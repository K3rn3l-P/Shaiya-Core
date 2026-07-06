// World Map Editor — place models + WLD export.
// Hooks:
//   FUN_005cee20 (AddModel, size=10): cattura entity+modelHandle al world load.
//   FUN_00450f30 (EntitySectionLoader, size=6): rileva tipo sezione.
// Preview: hijack entity esistente (già nel BSP), swap model handle + UpdateMatrix.
// Filename: letto da SModel+0x08 (copiato da ModelLoad/FUN_005d5780 a byte offset 8).
// WLD export: legge WLD originale, ricostruisce sezioni entity, copia il resto.

#include "include/imgui_layer_internal.h"
#include "include/shaiya/CPlayerData.h"
#include <external/stb/stb_image.h>
#include <util/util.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_set>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void naked_add_model_hook();
void naked_section_type_hook();

static unsigned s_addmodel_continue = 0x005cee2a;
static unsigned s_sectype_continue  = 0x00450f36;

// Mappa N (grid size): settato da FUN_00451860 durante LoadWorld.
static const int* s_mapN = (const int*)0x022f2280;

namespace {

enum class MapEntityType : uint8_t {
    Building = 0, Shape, Tree, Grass, Vani, Object, Unknown
};

struct CapturedEntity {
    void*         ptr;
    void*         modelHandle;
    MapEntityType type;
    float         pos[3];
    float         rot[3];    // rotFwd
    float         rotUp[3];
};

struct ModelEntry {
    void*         handle;
    MapEntityType type;
    int           index;
};

struct StagedHijack {
    void* entityPtr;
    void* origHandle;
    float origPos[3], origRot[3], origUp[3];
    void* newHandle;
    float pos[3];
    float rot[3];    // rotFwd corrente
    float rotUp[3];  // rotUp corrente
    float initRot[3]; // rotFwd iniziale (al momento del piazzamento)
    float initUp[3];  // rotUp iniziale
    MapEntityType type;
    int   modelIndex;
};

std::vector<CapturedEntity> g_ents;
std::vector<ModelEntry>     g_models;
std::vector<StagedHijack>   g_placements;
bool    g_modelsBuilt = false;
uint8_t g_currentType = (uint8_t)MapEntityType::Unknown;

// Posizioni dei placement salvati (persistenza cross-sessione via sidecar .mods).
// Usato da preview_show per non hijackare entity delle mod precedenti.
struct SavedMod { float pos[3]; };
static const uint32_t        kModsMagic     = 0x534F444Du; // "MODS"
static std::vector<SavedMod> s_savedMods;
static uint16_t              s_savedModsMapId = 0xFFFF;

// ── Wall snap settings (condivisi tra draw_gizmos e draw_inspector_panel) ──
float g_wallSnapWidth     = 10.0f; // fallback lunghezza (se AABB non disponibile)
float g_wallSnapDepth     = 2.0f;  // fallback spessore  (se AABB non disponibile)
float g_wallSnapThreshold = 3.0f;  // distanza di aggancio (unità)
bool  g_isSnapping        = false;
float g_snapPtX = 0.0f, g_snapPtZ = 0.0f;
float g_snapPtY = 0.0f; // altezza del punto di snap (per allineare Y)

} // namespace

typedef void  (__thiscall* UpdateMatrix_t)(void*);
static auto s_UpdateMatrix = (UpdateMatrix_t)0x005ce470;

// SModel::Load — __thiscall(this=ECX=SModel*, dir, filename)
typedef void (__thiscall* ModelLoad_t)(void*, const char*, const char*);
static auto s_ModelLoad = (ModelLoad_t)0x005d5780;

// ── Preview state ──
static void*  s_hijackEntity    = nullptr;
static void*  s_hijackOrigModel = nullptr;
static float  s_hijackOrigPos[3]= {};
static float  s_hijackOrigRot[3]= {};
static float  s_hijackOrigUp[3] = {};
static float  s_previewPos[3]   = {};
static bool   s_previewActive        = false;
static bool   s_alignToSlope         = false; // allinea rotazione alla normale del terreno (placement manuale)
static int    s_selectedPlacementIdx = -1; // indice in g_placements del placement attivo
static int    s_gizmoMode            = 0;  // 0=trasla 1=ruota (condiviso con Inspector)
static char   s_previewDbg[256] = "nessun modello selezionato";


// ── Hooks ──
static void __cdecl s_on_section(const char* dir)
{
    if (!dir) { g_currentType = (uint8_t)MapEntityType::Unknown; return; }
    if      (strstr(dir, "building")) {
        // "building" = prima sezione entity del LoadWorld → reload rilevato
        if (!g_ents.empty()) {
            g_ents.clear();
            g_placements.clear();
            g_modelsBuilt = false;
        }
        g_currentType = (uint8_t)MapEntityType::Building;
    }
    else if (strstr(dir, "shape"))    g_currentType = (uint8_t)MapEntityType::Shape;
    else if (strstr(dir, "tree"))     g_currentType = (uint8_t)MapEntityType::Tree;
    else if (strstr(dir, "grass"))    g_currentType = (uint8_t)MapEntityType::Grass;
    else if (strstr(dir, "vani"))     g_currentType = (uint8_t)MapEntityType::Vani;
    else if (strstr(dir, "object"))   g_currentType = (uint8_t)MapEntityType::Object;
    else                              g_currentType = (uint8_t)MapEntityType::Unknown;
}

void __declspec(naked) naked_section_type_hook()
{
    __asm {
        sub     esp, 0x13c
        pushad
        mov     eax, [esp + 0x160]
        push    eax
        call    s_on_section
        add     esp, 4
        popad
        jmp     s_sectype_continue
    }
}

// callerRetAddr: indirizzo di ritorno del caller di FUN_005cee20 nel call stack.
// Solo entity caricate da FUN_00450f30 (0x00450f30..0x0045118F) sono valide —
// altri loader (mani 0x004242ba, object 0x00568xxx) usano la stessa funzione ma
// non devono essere catturate perché corrompono i conteggi delle sezioni WLD.
static void __cdecl s_on_entity(void* entityPtr, uintptr_t callerRetAddr)
{
    if (callerRetAddr < 0x00450f30u || callerRetAddr > 0x0045118fu) return;
    if (!entityPtr || (uintptr_t)entityPtr < 0x10000) return;
    auto* b = (uint8_t*)entityPtr;
    CapturedEntity e{};
    e.ptr         = entityPtr;
    e.modelHandle = *(void**)(b + 0xA0);
    e.type        = (MapEntityType)g_currentType;
    e.pos[0]   = *(float*)(b + 0x04);
    e.pos[1]   = *(float*)(b + 0x08);
    e.pos[2]   = *(float*)(b + 0x0c);
    e.rot[0]   = *(float*)(b + 0x10);
    e.rot[1]   = *(float*)(b + 0x14);
    e.rot[2]   = *(float*)(b + 0x18);
    e.rotUp[0] = *(float*)(b + 0x1c);
    e.rotUp[1] = *(float*)(b + 0x20);
    e.rotUp[2] = *(float*)(b + 0x24);
    g_ents.push_back(e);
    g_modelsBuilt = false;
}

void __declspec(naked) naked_add_model_hook()
{
    __asm {
        mov     eax, dword ptr [esp + 0x4]
        mov     dword ptr [ecx + 0xa0], eax
        pushad
        // Dopo pushad (8 reg × 4 byte = 0x20 byte), il return address originale
        // di FUN_005cee20 è ora a [esp+0x20].
        mov     eax, [esp + 0x20]    // callerRetAddr
        push    eax                   // arg2: callerRetAddr
        push    ecx                   // arg1: entityPtr
        call    s_on_entity
        add     esp, 8
        popad
        jmp     s_addmodel_continue
    }
}

void hook::map_editor()
{
    util::detour((void*)0x005cee20, naked_add_model_hook,    10);
    util::detour((void*)0x00450f30, naked_section_type_hook,  6);
}

// ── WLD export ──
namespace {

static const char* model_filename(void* handle)
{
    if (!handle || (uintptr_t)handle < 0x10000) return nullptr;
    const char* fn = (const char*)((uint8_t*)handle + 8);
    if (fn[0] == '\0' || fn[0] == '\xFF') return nullptr;
    return fn;
}

// Legge AABB da SModel+0x118 (6 float: minX,minY,minZ,maxX,maxY,maxZ)
// Formato 3DC: bytes 16-39 dell'header = AABB min+max in model space
static bool get_model_aabb(void* handle, float out[6])
{
    // Solo i nostri slot VirtualAlloc (sempre page-aligned, 0x1000).
    // Handle del pool interno del gioco hanno offset sub-pagina → crash a +0x118.
    uintptr_t h = (uintptr_t)handle;
    if (!h || h < 0x00010000 || (h & 0xFFFu) != 0) return false;
    memcpy(out, (uint8_t*)handle + 0x118, 24);
    // Sanity: almeno un asse deve avere min < max e valori ragionevoli
    bool ok = (out[0] < out[3] || out[1] < out[4] || out[2] < out[5]);
    if (!ok) return false;
    // Valori fuori range suggeriscono offset sbagliato
    for (int i = 0; i < 6; i++)
        if (out[i] < -10000.f || out[i] > 10000.f) return false;
    return true;
}

// Restituisce le mezze-estensioni del modello lungo fwd e right
// usando AABB in model space proiettata sulla direzione del muro
static void get_wall_extents(void* handle, float fwdX, float fwdZ,
                              float& extFwd, float& extRight,
                              float fallbackFwd, float fallbackRight)
{
    float aabb[6];
    if (get_model_aabb(handle, aabb)) {
        float hx = (aabb[3] - aabb[0]) * 0.5f;
        float hz = (aabb[5] - aabb[2]) * 0.5f;
        // Support function del rettangolo lungo le due direzioni
        extFwd   = fabsf(fwdX) * hx + fabsf(fwdZ) * hz;
        extRight = fabsf(fwdZ) * hx + fabsf(fwdX) * hz;
    } else {
        extFwd   = fallbackFwd;
        extRight = fallbackRight;
    }
}

static void write_entity_section(FILE* out, MapEntityType stype)
{
    std::vector<std::string> catalogue;
    auto cat_idx = [&](void* handle) -> int32_t {
        const char* fn = model_filename(handle);
        if (!fn) fn = "unknown.3DC";
        for (int i = 0; i < (int)catalogue.size(); i++)
            if (catalogue[i] == fn) return i;
        catalogue.push_back(fn);
        return (int32_t)catalogue.size() - 1;
    };

    auto matches = [&](MapEntityType t) -> bool {
        if (t == stype) return true;
        if (stype == MapEntityType::Grass && t == MapEntityType::Vani) return true;
        return false;
    };

    for (auto& e : g_ents)
        if (matches(e.type) && e.modelHandle) cat_idx(e.modelHandle);
    for (auto& h : g_placements)
        if (matches(h.type) && h.newHandle) cat_idx(h.newHandle);

    int32_t mc = (int32_t)catalogue.size();
    fwrite(&mc, 4, 1, out);
    for (auto& name : catalogue) {
        char buf[256] = {};
        strncpy_s(buf, sizeof(buf), name.c_str(), _TRUNCATE);
        fwrite(buf, 256, 1, out);
    }

    // ic deve contare solo entità con modelHandle valido (stesso filtro del catalogue).
    // Se ic includesse entità con handle null, cat_idx(null) aggiungerebbe "unknown.3DC"
    // al catalogue DOPO che mc è già stato scritto → indice fuori bounds → crash.
    int32_t ic = 0;
    for (auto& e : g_ents)       if (matches(e.type) && e.modelHandle) ic++;
    for (auto& h : g_placements) if (matches(h.type) && h.newHandle)   ic++;
    fwrite(&ic, 4, 1, out);

    for (auto& e : g_ents) {
        if (!matches(e.type) || !e.modelHandle) continue;
        int32_t idx = cat_idx(e.modelHandle);
        fwrite(&idx,    4, 1, out);
        fwrite(e.pos,   4, 3, out);
        fwrite(e.rot,   4, 3, out);
        fwrite(e.rotUp, 4, 3, out);
    }

    for (auto& h : g_placements) {
        if (!matches(h.type) || !h.newHandle) continue;
        int32_t idx = cat_idx(h.newHandle);
        fwrite(&idx,    4, 1, out);
        fwrite(h.pos,   4, 3, out);
        fwrite(h.rot,   4, 3, out);
        fwrite(h.rotUp, 4, 3, out);
    }
}

static size_t compute_entity_start(const uint8_t* data, size_t fsz)
{
    int N = *s_mapN;
    if (N <= 0 || N > 8192) return 0;
    size_t terrTypeCountOff = 8 + (size_t)N * N * 3;
    if (terrTypeCountOff + 4 > fsz) return 0;
    int32_t terrTypeCount = *(int32_t*)(data + terrTypeCountOff);
    if (terrTypeCount < 0 || terrTypeCount > 256) return 0;
    size_t entityStart = terrTypeCountOff + 4 + (size_t)terrTypeCount * 516 + 256;
    if (entityStart > fsz) return 0;
    return entityStart;
}

static size_t skip_four_entity_sections(const uint8_t* data, size_t off, size_t fsz)
{
    for (int i = 0; i < 4; i++) {
        if (off + 4 > fsz) return fsz;
        int32_t mc = *(int32_t*)(data + off); off += 4;
        if (mc < 0 || mc > 65536) return fsz;
        off += (size_t)mc * 256;
        if (off + 4 > fsz) return fsz;
        int32_t ic = *(int32_t*)(data + off); off += 4;
        if (ic < 0 || ic > 1000000) return fsz;
        off += (size_t)ic * 40;
    }
    return off;
}

static char s_exportMsg[320] = "Premi 'Esporta WLD' per salvare.";
static char s_pathSrc[MAX_PATH] = {};
static char s_pathDst[MAX_PATH] = {};
static char s_lastSavedPath[MAX_PATH] = {};

static bool get_dll_dir(char* out, size_t outSz)
{
    HMODULE hm = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&get_dll_dir, &hm);
    if (!hm) return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(hm, path, MAX_PATH)) return false;
    char* last = strrchr(path, '\\');
    if (!last) return false;
    *last = '\0';
    strncpy_s(out, outSz, path, _TRUNCATE);
    return true;
}

static uint16_t get_current_map_id()
{
    if (!g_pPlayerData) return 0xFFFF;
    return g_pPlayerData->mapId;
}

// Il Map Editor legge gli asset dalla cartella "Data" estratta accanto alla DLL.
// Con i client che spediscono solo data.saf/sah (Data non estratta) tutte le
// texture/modelli falliscono silenziosamente. Questo check permette di avvisare.
static bool data_folder_present()
{
    char dllDir[MAX_PATH] = {};
    if (!get_dll_dir(dllDir, sizeof(dllDir))) return true; // non bloccare se path ignoto
    char probe[MAX_PATH] = {};
    snprintf(probe, sizeof(probe), "%s\\Data\\World", dllDir);
    DWORD a = GetFileAttributesA(probe);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool build_wld_paths(char* srcOut, size_t srcSz, char* dstOut, size_t dstSz, uint16_t mapId)
{
    char dllDir[MAX_PATH] = {};
    if (!get_dll_dir(dllDir, sizeof(dllDir))) return false;
    snprintf(srcOut, srcSz, "%s\\Data\\World\\%u.wld",  dllDir, (unsigned)mapId);
    snprintf(dstOut, dstSz, "%s\\Output_Maps\\World\\%u.wld", dllDir, (unsigned)mapId);
    return true;
}

static void ensure_dir(const char* filePath)
{
    char dir[MAX_PATH] = {};
    strncpy_s(dir, sizeof(dir), filePath, _TRUNCATE);
    char* last = strrchr(dir, '\\');
    if (!last) return;
    *last = '\0';
    for (char* p = dir + 1; *p; p++) {
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(dir, nullptr);
            *p = '\\';
        }
    }
    CreateDirectoryA(dir, nullptr);
}

// ── Sidecar .mods: persistenza posizioni placement cross-sessione ─────────
// Dopo un reload del WLD il gioco ricrea le entity con handle interni nuovi.
// g_placements viene azzerato (pointer invalidi). Senza il sidecar, preview_show
// non saprebbe quali entity sono "nostre" → le hijackerebbe → mod scompaiono.

static char s_modsPath[MAX_PATH] = {};

static void saved_mods_build_path(uint16_t mapId)
{
    char dllDir[MAX_PATH] = {};
    if (!get_dll_dir(dllDir, sizeof(dllDir))) return;
    snprintf(s_modsPath, sizeof(s_modsPath),
        "%s\\Output_Maps\\%u.mods", dllDir, (unsigned)mapId);
}

static void saved_mods_load(uint16_t mapId)
{
    if (s_savedModsMapId == mapId) return;
    s_savedMods.clear();
    s_savedModsMapId = mapId;
    saved_mods_build_path(mapId);
    if (!s_modsPath[0]) return;

    FILE* f = nullptr;
    fopen_s(&f, s_modsPath, "rb");
    if (!f) return;

    uint32_t magic = 0, count = 0;
    fread(&magic, 4, 1, f);
    fread(&count, 4, 1, f);
    if (magic != kModsMagic || count > 500000) { fclose(f); s_savedMods.clear(); return; }

    s_savedMods.resize(count);
    for (uint32_t i = 0; i < count; i++)
        fread(s_savedMods[i].pos, 4, 3, f);
    fclose(f);
}

static void saved_mods_save(uint16_t mapId)
{
    saved_mods_build_path(mapId);
    if (!s_modsPath[0]) return;
    ensure_dir(s_modsPath);

    // Parti dai mods già in memoria (sessioni precedenti)
    std::vector<SavedMod> all = s_savedMods;

    // Aggiungi le posizioni dei placement correnti (evita duplicati)
    for (auto& h : g_placements) {
        bool dup = false;
        for (auto& sp : all)
            if (fabsf(sp.pos[0]-h.pos[0]) < 0.5f &&
                fabsf(sp.pos[1]-h.pos[1]) < 0.5f &&
                fabsf(sp.pos[2]-h.pos[2]) < 0.5f)
                { dup = true; break; }
        if (!dup) {
            SavedMod sm{};
            memcpy(sm.pos, h.pos, 12);
            all.push_back(sm);
        }
    }

    FILE* f = nullptr;
    fopen_s(&f, s_modsPath, "wb");
    if (!f) return;
    uint32_t magic = kModsMagic;
    uint32_t count = (uint32_t)all.size();
    fwrite(&magic, 4, 1, f);
    fwrite(&count, 4, 1, f);
    for (auto& sp : all)
        fwrite(sp.pos, 4, 3, f);
    fclose(f);

    s_savedMods     = all;
    s_savedModsMapId = mapId;
}

static bool saved_mods_contains(const float pos[3])
{
    const float kEps = 0.5f;
    for (auto& sp : s_savedMods)
        if (fabsf(sp.pos[0]-pos[0]) < kEps &&
            fabsf(sp.pos[1]-pos[1]) < kEps &&
            fabsf(sp.pos[2]-pos[2]) < kEps)
            return true;
    return false;
}
// ─────────────────────────────────────────────────────────────────────────────

static void do_export_wld()
{
    int N = *s_mapN;
    if (N <= 0) {
        snprintf(s_exportMsg, sizeof(s_exportMsg), "ERRORE: N=%d — mappa non caricata.", N);
        return;
    }
    uint16_t mapId = get_current_map_id();
    if (mapId == 0xFFFF) {
        snprintf(s_exportMsg, sizeof(s_exportMsg), "ERRORE: CPlayerData null (non loggato?).");
        return;
    }

    // Ricostruisce sempre i path dal mapId corrente — immune da globals stale/svuotati
    char pathSrc[MAX_PATH] = {}, pathDst[MAX_PATH] = {};
    if (!build_wld_paths(pathSrc, sizeof(pathSrc), pathDst, sizeof(pathDst), mapId)) {
        snprintf(s_exportMsg, sizeof(s_exportMsg), "ERRORE: impossibile calcolare path DLL.");
        return;
    }
    strncpy_s(s_pathSrc, sizeof(s_pathSrc), pathSrc, _TRUNCATE);
    strncpy_s(s_pathDst, sizeof(s_pathDst), pathDst, _TRUNCATE);

    FILE* f = nullptr;
    fopen_s(&f, pathSrc, "rb");
    if (!f) {
        snprintf(s_exportMsg, sizeof(s_exportMsg),
            "ERRORE: '%s' non trovato.", pathSrc);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsz_l = ftell(f); rewind(f);
    if (fsz_l <= 0) { fclose(f); snprintf(s_exportMsg, sizeof(s_exportMsg), "ERRORE: WLD vuoto."); return; }
    size_t fsz = (size_t)fsz_l;
    std::vector<uint8_t> orig(fsz);
    fread(orig.data(), 1, fsz, f);
    fclose(f);

    // Patch terrain height+type da RAM (stesso effetto di terra_save_wld)
    {
        int16_t* hm_ptr = *(int16_t**)0x0090169C;
        uint8_t* tm_ptr = *(uint8_t**)0x009016A0;
        size_t hmSz = (size_t)N * N * 2;
        size_t tmSz = (size_t)N * N;
        if (hm_ptr && fsz >= 8 + hmSz + tmSz) {
            memcpy(orig.data() + 8, hm_ptr, hmSz);
            if (tm_ptr) memcpy(orig.data() + 8 + hmSz, tm_ptr, tmSz);
        }
    }

    // Se nessuna entity è stata modificata/aggiunta, scrivi orig verbatim
    // (solo terrain già patchato sopra). Evita bug di parsing sezioni entity.
    if (g_placements.empty()) {
        ensure_dir(pathDst);
        FILE* out = nullptr;
        fopen_s(&out, pathDst, "wb");
        if (!out) {
            snprintf(s_exportMsg, sizeof(s_exportMsg), "ERRORE: impossibile creare '%s'", pathDst);
            return;
        }
        fwrite(orig.data(), 1, fsz, out);
        fclose(out);
        strncpy_s(s_lastSavedPath, sizeof(s_lastSavedPath), pathDst, _TRUNCATE);
        saved_mods_save(mapId);
        snprintf(s_exportMsg, sizeof(s_exportMsg),
            "OK — Map %u  N=%d  terrain patchato, entità originali intatte.",
            (unsigned)mapId, N);
        return;
    }

    size_t entityStart = compute_entity_start(orig.data(), fsz);
    if (entityStart == 0) {
        snprintf(s_exportMsg, sizeof(s_exportMsg),
            "ERRORE: calcolo entityStart fallito (N=%d fsz=%zu).", N, fsz);
        return;
    }

    size_t afterEntities = skip_four_entity_sections(orig.data(), entityStart, fsz);
    ensure_dir(pathDst);

    FILE* out = nullptr;
    fopen_s(&out, pathDst, "wb");
    if (!out) {
        snprintf(s_exportMsg, sizeof(s_exportMsg), "ERRORE: impossibile creare '%s'", pathDst);
        return;
    }

    fwrite(orig.data(), 1, entityStart, out);

    static const MapEntityType kSections[] = {
        MapEntityType::Building, MapEntityType::Shape,
        MapEntityType::Tree,     MapEntityType::Grass
    };
    for (auto stype : kSections)
        write_entity_section(out, stype);

    if (afterEntities < fsz)
        fwrite(orig.data() + afterEntities, 1, fsz - afterEntities, out);

    fclose(out);
    strncpy_s(s_lastSavedPath, sizeof(s_lastSavedPath), pathDst, _TRUNCATE);

    int origEnts = 0;
    for (auto& e : g_ents)
        if (e.type != MapEntityType::Object && e.type != MapEntityType::Unknown) origEnts++;
    saved_mods_save(mapId);
    snprintf(s_exportMsg, sizeof(s_exportMsg),
        "OK — Map %u  N=%d  entity orig=%d  nuove=%d  (terrain+entities)",
        (unsigned)mapId, N, origEnts, (int)g_placements.size());
}

// ── Filesystem model scanner ──
static MapEntityType dir_to_type(const char* dirname)
{
    char low[64] = {};
    for (int i = 0; dirname[i] && i < 63; i++)
        low[i] = (char)tolower((unsigned char)dirname[i]);
    if (strstr(low, "building")) return MapEntityType::Building;
    if (strstr(low, "shape"))    return MapEntityType::Shape;
    if (strstr(low, "tree"))     return MapEntityType::Tree;
    if (strstr(low, "grass"))    return MapEntityType::Grass;
    if (strstr(low, "vani"))     return MapEntityType::Vani;
    if (strstr(low, "object"))   return MapEntityType::Object;
    return MapEntityType::Unknown;
}

struct FsModel {
    char          filename[128];
    char          relDir[64];
    MapEntityType type;
    void*         handle;
    bool          thumbTried; // true = auto-load già tentato (evita retry infinito)
};
static std::vector<FsModel> s_fsModels;
static bool s_fsScanned = false;

static void scan_entity_subdir(const char* dirPath, const char* dirName)
{
    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", dirPath);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext) continue;
        char exl[8] = {};
        for (int i = 0; ext[i] && i < 7; i++)
            exl[i] = (char)tolower((unsigned char)ext[i]);
        if (strcmp(exl, ".smod") != 0 && strcmp(exl, ".3dc") != 0) continue;
        FsModel fm{};
        strncpy_s(fm.filename, sizeof(fm.filename), fd.cFileName, _TRUNCATE);
        strncpy_s(fm.relDir,   sizeof(fm.relDir),   dirName,      _TRUNCATE);
        fm.type       = dir_to_type(dirName);
        fm.handle     = nullptr;
        fm.thumbTried = false;
        s_fsModels.push_back(fm);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void rebuild_fs_models()
{
    s_fsModels.clear();
    char dllDir[MAX_PATH] = {};
    if (!get_dll_dir(dllDir, sizeof(dllDir))) return;
    char entityDir[MAX_PATH];
    snprintf(entityDir, MAX_PATH, "%s\\Data\\Entity", dllDir);

    char pattern[MAX_PATH];
    snprintf(pattern, MAX_PATH, "%s\\*", entityDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char subPath[MAX_PATH];
        snprintf(subPath, MAX_PATH, "%s\\%s", entityDir, fd.cFileName);
        scan_entity_subdir(subPath, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(s_fsModels.begin(), s_fsModels.end(),
        [](const FsModel& a, const FsModel& b) {
            return _stricmp(a.filename, b.filename) < 0;
        });
    s_fsScanned = true;
}

static const char* type_to_dir(MapEntityType t)
{
    switch (t) {
    case MapEntityType::Building: return "data/entity/building";
    case MapEntityType::Shape:    return "data/entity/shape";
    case MapEntityType::Tree:     return "data/entity/tree";
    case MapEntityType::Grass:    return "data/entity/grass";
    case MapEntityType::Vani:     return "data/entity/vani";
    case MapEntityType::Object:   return "data/entity/object";
    default:                      return "data/entity/building";
    }
}

// ── LRU thumbnail pool ────────────────────────────────────────────────────────
// Max 60 slot VirtualAlloc fissi (mai liberati), riutilizzati con eviction LRU.
// Quando un slot viene evictato: Release() delle texture D3D9 → memset → ricarica.
// Le texture D3D9 sono create da noi (via s_ModelLoad su slot custom), quindi
// il gioco non ha AddRef aggiuntivi → Release() le libera correttamente.
// SModelTexture stride = 0x20 byte (confermato da FUN_005d5600 decompiler).
// Slot "pinnati" = usati da g_placements.newHandle → mai evictati.

static const int kPoolMax = 48;

struct ThumbSlot {
    char     filename[128]; // '\0' = slot libero/evictato
    void*    mem;           // VirtualAlloc 0x164; null = non ancora allocato
    uint32_t lastFrame;     // ImGui frame dell'ultimo accesso (LRU key)
};
static ThumbSlot s_pool[kPoolMax] = {};

static bool thumb_is_pinned(void* mem)
{
    for (auto& h : g_placements)
        if (h.newHandle == mem) return true;
    // After WLD reload g_placements is cleared, but world entities still show our pool handles.
    // Pin those slots too so the LRU sweeper never recycles memory that's live in-game.
    for (auto& e : g_ents)
        if (e.modelHandle == mem) return true;
    return false;
}

// FUN_005d5360 = SModel cleanup ufficiale del gioco (__fastcall, ECX = SModel*).
// Chiama FUN_005d5050 su ogni SModelTexture (destructor: Release VB, Release IB,
// decrref STexture nel texture manager) poi libera l'heap del texArr e azzera slot[0..1].
// Verificato con Ghidra: è l'unica funzione che decrementa correttamente il ref
// delle STexture nella cache condivisa → unico modo sicuro per non saturare la VRAM.
typedef void (__fastcall* SModelDestroy_t)(void*);
static const SModelDestroy_t s_SModelDestroy = (SModelDestroy_t)0x005d5360;

static void thumb_slot_destroy(void* mem)
{
    if (!mem) return;
    // Chiama solo se numMeshes > 0 (texArr allocato) — evita chiamata su slot vuoto
    if (*(int32_t*)mem <= 0) return;
    s_SModelDestroy(mem);
}

// Evict singolo slot + invalida tutti fm.handle stale che puntavano ad esso.
static void thumb_evict_slot(ThumbSlot& s)
{
    void* mem = s.mem;
    thumb_slot_destroy(mem);
    memset(mem, 0, 0x164);
    s.filename[0] = '\0';
    s.lastFrame   = 0;
    for (auto& fm : s_fsModels)
        if (fm.handle == mem) { fm.handle = nullptr; fm.thumbTried = false; }
}

// Libera slot non pinnati idle da più di maxIdleFrames (eviction proattiva).
static void thumb_pool_sweep(uint32_t currentFrame, uint32_t maxIdleFrames)
{
    for (auto& s : s_pool) {
        if (!s.mem || !s.filename[0]) continue;
        if (thumb_is_pinned(s.mem)) continue;
        if ((currentFrame - s.lastFrame) <= maxIdleFrames) continue;
        thumb_evict_slot(s);
    }
}

// SEH wrapper: cattura crash D3D dentro s_ModelLoad (es. CreateIndexBuffer fallisce).
static bool try_model_load(void* mem, const char* dir, const char* filename)
{
    __try { s_ModelLoad(mem, dir, filename); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Libera slot thumbnail non pinnati (Ricarica / cambio sezione).
static void thumb_pool_evict_all()
{
    for (auto& s : s_pool) {
        if (!s.mem || !s.filename[0]) continue;
        if (thumb_is_pinned(s.mem)) continue;
        thumb_evict_slot(s);
    }
    // Reset modelli con caricamento fallito (handle=null, thumbTried=true) → riprova
    for (auto& fm : s_fsModels)
        if (!fm.handle) fm.thumbTried = false;
}

static void* dyn_load_model(MapEntityType type, const char* filename)
{
    if (!filename || filename[0] == '\0') return nullptr;
    uint32_t f = (uint32_t)ImGui::GetFrameCount();

    // 1. Già in pool → aggiorna LRU e ritorna
    for (auto& s : s_pool) {
        if (s.mem && s.filename[0] && _stricmp(s.filename, filename) == 0) {
            s.lastFrame = f;
            return s.mem;
        }
    }

    // 2. Slot libero (mem non ancora allocato o filename vuoto)
    ThumbSlot* target = nullptr;
    for (auto& s : s_pool)
        if (!s.mem || !s.filename[0]) { target = &s; break; }

    // 3. Nessun slot libero → evict LRU non pinnato
    if (!target) {
        uint32_t oldest = UINT32_MAX;
        for (auto& s : s_pool) {
            if (s.mem && !thumb_is_pinned(s.mem) && s.lastFrame < oldest) {
                oldest = s.lastFrame; target = &s;
            }
        }
    }
    if (!target) return nullptr; // tutti i slot sono pinnati

    // 4. Alloca VirtualAlloc la prima volta; al riuso evict il vecchio slot e invalida handle stale
    if (!target->mem) {
        target->mem = VirtualAlloc(nullptr, 0x164, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!target->mem) return nullptr;
    } else {
        thumb_evict_slot(*target); // evict + invalida fm.handle stale che puntavano a questo slot
    }

    // 5. Carica nuovo modello (SEH-protected: se D3D crasha, slot resta vuoto)
    memset(target->mem, 0, 0x164);
    if (!try_model_load(target->mem, type_to_dir(type), filename)) {
        target->filename[0] = '\0'; target->lastFrame = 0;
        return nullptr;
    }

    const char* fn = model_filename(target->mem);
    if (!fn || fn[0] == '\0') {
        // Load fallita: slot resta allocato vuoto, pronto per il prossimo modello
        target->filename[0] = '\0'; target->lastFrame = 0;
        return nullptr;
    }

    strncpy_s(target->filename, sizeof(target->filename), filename, _TRUNCATE);
    target->lastFrame = f;
    return target->mem;
}

static void match_fs_handles()
{
    for (auto& fm : s_fsModels) {
        fm.handle = nullptr;
        for (auto& m : g_models) {
            const char* fn = model_filename(m.handle);
            if (fn && _stricmp(fn, fm.filename) == 0) { fm.handle = m.handle; break; }
        }
        if (!fm.handle) {
            for (auto& s : s_pool) {
                if (s.mem && s.filename[0] && _stricmp(s.filename, fm.filename) == 0)
                    { fm.handle = s.mem; break; }
            }
        }
    }
}

// Ottieni la prima texture D3D9 da un SModel caricato.
// SModel+0x00 = numTextures, +0x04 = SModelTexture*, SModelTexture+0x1C = STexture*, STexture+0x204 = LPDIRECT3DTEXTURE9
static IDirect3DTexture9* get_model_thumb(void* handle)
{
    if (!handle || (uintptr_t)handle < 0x10000) return nullptr;
    int32_t numTex = *(int32_t*)handle;
    if (numTex <= 0) return nullptr;
    auto* texArr = *(uint8_t**)((uint8_t*)handle + 4);
    if (!texArr || (uintptr_t)texArr < 0x10000) return nullptr;
    auto* stex = *(uint8_t**)(texArr + 0x1c);
    if (!stex || (uintptr_t)stex < 0x10000) return nullptr;
    return *(IDirect3DTexture9**)(stex + 0x204);
}

// D3D9 device pointer (confermato: DAT_022b69a8)
static IDirect3DDevice9** const ppD3DDevice = (IDirect3DDevice9**)0x022b69a8;

// Proietta un punto world 3D → screen 2D (D3D9 LH, row-vector convention).
// Ritorna false se il punto è dietro la camera.
static bool world_to_screen(
    float wx, float wy, float wz,
    const D3DMATRIX& view, const D3DMATRIX& proj,
    const D3DVIEWPORT9& vp, ImVec2& out)
{
    float vx = wx*view._11 + wy*view._21 + wz*view._31 + view._41;
    float vy = wx*view._12 + wy*view._22 + wz*view._32 + view._42;
    float vz = wx*view._13 + wy*view._23 + wz*view._33 + view._43;
    float vw = wx*view._14 + wy*view._24 + wz*view._34 + view._44;
    float cx  = vx*proj._11 + vy*proj._21 + vz*proj._31 + vw*proj._41;
    float cy  = vx*proj._12 + vy*proj._22 + vz*proj._32 + vw*proj._42;
    float cw  = vx*proj._14 + vy*proj._24 + vz*proj._34 + vw*proj._44;
    if (cw < 0.001f) return false;
    float nx = cx / cw, ny = cy / cw;
    out.x = (1.0f + nx) * 0.5f * (float)vp.Width  + (float)vp.X;
    out.y = (1.0f - ny) * 0.5f * (float)vp.Height + (float)vp.Y;
    return true;
}

} // namespace

// ── Draw ──
namespace imgui_layer {

    enum class EditorSection : int {
        None = -1, Build = 0, Trees, Grass, Vani, Props, Wall, Others, Save, Existing, Terra, Scatter
    };
    static EditorSection s_section    = EditorSection::None;
    static int           s_contentSel = -1;

    // ── Terra Editor state ────────────────────────────────────────────
    static IDirect3DTexture9* s_terraCanvasTex   = nullptr;
    static bool               s_terraCanvasDirty = true;
    static float              s_terraBrushR      = 5.0f;   // raggio in celle heightmap
    static float              s_terraBrushStr    = 40.0f;  // delta int16 per frame
    static int                s_terraBrushMode   = 0;      // 0=alza 1=abbassa 2=smooth 3=tipo
    static int                s_terraPaintType   = 0;
    static bool               s_terraShowTypes   = false;
    static bool               s_terraHasEdits    = false;
    static char               s_terraMsg[256]    = {};
    // Minimap (Data\Interface\MiniMap\{mapId}.png)
    static IDirect3DTexture9* s_terraMiniTex     = nullptr;
    static int                s_terraMiniW       = 0;
    static int                s_terraMiniH       = 0;
    static uint16_t           s_terraMiniMapId   = 0xFFFF;
    // Terrain type names letti dal WLD
    static char               s_terraTypeNames[16][260] = {};
    static int                s_terraTypeCount   = 0;
    static uint16_t           s_terraWldMapId    = 0xFFFF;
    // Undo/redo terrain
    struct TerraUndoStroke {
        std::vector<std::pair<int,int16_t>> hmOld;
        std::vector<std::pair<int,uint8_t>> tmOld;
    };
    static std::vector<TerraUndoStroke>  s_undoStack;
    static bool                          s_strokeOpen   = false;
    static std::unordered_set<int>       s_hmRecorded;
    static std::unordered_set<int>       s_tmRecorded;
    static const int                     kMaxUndo       = 32;
    // Snapshot originale per "Annulla tutto"
    static std::vector<int16_t>          s_terraOrigHM;
    static std::vector<uint8_t>          s_terraOrigTM;
    static int                           s_terraOrigN   = 0;
    // Zoom/pan minimap
    static float                         s_miniZoom     = 1.0f;
    static float                         s_miniPanX     = 0.0f;
    static float                         s_miniPanZ     = 0.0f;

    // ── Scatter Erba state ────────────────────────────────────────────
    static float s_scBrushR       = 8.0f;
    static float s_scDensity      = 0.30f;
    static float s_scMinSpacing   = 4.0f;
    static bool  s_scRotY         = true;
    static bool  s_scAlignSlope   = true;  // allinea rotUp alla normale del terreno
    static bool  s_scTypeFilter[16] = {};
    static bool  s_scFilterInited   = false;
    static int   s_scModelSel       = -1;
    static char  s_scMsg[256]       = {};
    static std::vector<int> s_scUndoBatches;
    static int   s_scBatchStart     = -1;
    static bool  s_scDragging       = false;
    static float s_scMiniZoom       = 1.0f;
    static float s_scMiniPanX       = 0.0f;
    static float s_scMiniPanZ       = 0.0f;

    // Stato modifica entità già presenti nella mappa (g_ents)
    static int  s_selectedEntIdx  = -1;   // indice in g_ents (-1 = nessuna)
    static bool s_editingExisting = false; // true = gizmo su entità pre-esistente

    static const float kSidebarW  = 90.0f;
    static float       s_contentW = 360.0f; // ridimensionabile via drag handle

    static const char* type_str(MapEntityType t)
    {
        switch (t) {
        case MapEntityType::Building: return "Bld";
        case MapEntityType::Shape:    return "Shp";
        case MapEntityType::Tree:     return "Tre";
        case MapEntityType::Grass:    return "Grs";
        case MapEntityType::Vani:     return "Van";
        case MapEntityType::Object:   return "Obj";
        default:                      return "Unk";
        }
    }

    static const char* type_str_full(MapEntityType t)
    {
        switch (t) {
        case MapEntityType::Building: return "Building";
        case MapEntityType::Shape:    return "Shape";
        case MapEntityType::Tree:     return "Tree";
        case MapEntityType::Grass:    return "Grass";
        case MapEntityType::Vani:     return "Vani";
        case MapEntityType::Object:   return "Object";
        default:                      return "Unknown";
        }
    }

    static MapEntityType section_to_type(EditorSection s)
    {
        switch (s) {
        case EditorSection::Build:  return MapEntityType::Building;
        case EditorSection::Props:  return MapEntityType::Shape;
        case EditorSection::Wall:   return MapEntityType::Shape; // subset di Shape filtrato per "wall"
        case EditorSection::Trees:  return MapEntityType::Tree;
        case EditorSection::Grass:  return MapEntityType::Grass;
        case EditorSection::Vani:   return MapEntityType::Vani;
        case EditorSection::Others: return MapEntityType::Object;
        default:                    return MapEntityType::Unknown;
        }
    }

    static void build_model_list()
    {
        if (g_modelsBuilt) return;
        g_modelsBuilt = true;
        g_models.clear();
        int idx = 0;
        for (auto& e : g_ents) {
            if (!e.modelHandle) continue;
            bool exists = false;
            for (auto& m : g_models)
                if (m.handle == e.modelHandle) { exists = true; break; }
            if (!exists)
                g_models.push_back({ e.modelHandle, e.type, idx++ });
        }
    }

    // ── Terra helpers ────────────────────────────────────────────────────
    // Indirizzi terrain system (verificati Ghidra game.exe):
    //   CWorldMgr singleton    = 0x7C4A68  (thiscall target)
    //   terrain struct         = CWorldMgr+0xC = 0x7C4A74  (LEA ECX,[EBP+0xC])
    //   heightMap ptr-to-ptr   = terrainBase + 0x13CC28
    //   typeMap   ptr-to-ptr   = terrainBase + 0x13CC2C
    //   N (grid vertex count)  = 0x022F2280  (DAT_022f2280)
    //   vertex world spacing   = 0x022F227C  (DAT_022f227c, float)

    static int16_t* terra_hm()
    {
        auto** p = (int16_t**)(0x7C4A74 + 0x13CC28);
        int16_t* h = *p;
        return ((uintptr_t)h > 0xFFFFu) ? h : nullptr;
    }
    static uint8_t* terra_tm()
    {
        auto** p = (uint8_t**)(0x7C4A74 + 0x13CC2C);
        uint8_t* t = *p;
        return ((uintptr_t)t > 0xFFFFu) ? t : nullptr;
    }
    static int terra_N()
    {
        int n = *(int*)0x022F2280;
        return (n > 0 && n <= 4096) ? n : 0;
    }

    // Normale del terreno a (worldX, worldZ) — derivata finita dall'heightmap.
    // Ritorna vettore normalizzato (nx,ny,nz) che punta perpendicolare alla superficie.
    static void terrain_normal_at(float worldX, float worldZ, float& nx, float& ny, float& nz)
    {
        int N = terra_N(); int16_t* hm = terra_hm();
        float vs = *(const float*)0x022F227C;
        float hs = *(const float*)0x022F2278;
        if (N <= 0 || !hm || vs <= 0.0f) { nx=0.0f; ny=1.0f; nz=0.0f; return; }
        int cx = max(1, min(N-2, (int)(worldX / vs + 0.5f)));
        int cz = max(1, min(N-2, (int)(worldZ / vs + 0.5f)));
        float hL = (float)(uint16_t)hm[(size_t)cz*N+(cx-1)] * hs;
        float hR = (float)(uint16_t)hm[(size_t)cz*N+(cx+1)] * hs;
        float hB = (float)(uint16_t)hm[(size_t)(cz-1)*N+cx] * hs;
        float hF = (float)(uint16_t)hm[(size_t)(cz+1)*N+cx] * hs;
        nx = -(hR-hL)/(2.0f*vs); ny = 1.0f; nz = -(hF-hB)/(2.0f*vs);
        float len = sqrtf(nx*nx+ny*ny+nz*nz);
        if (len > 0.0001f) { nx/=len; ny/=len; nz/=len; } else { nx=0;ny=1;nz=0; }
    }

    // worldX,Z → worldY con la stessa formula del gioco (FUN_005c9110 + 005D9F37).
    // MOVZX (zero-extend → uint16) + bilinear interp + hs + yOff.
    // Non chiama FUN_005c9110 direttamente: ECX = terrain object può essere null dall'hook.
    static float terrain_height_at(float worldX, float worldZ)
    {
        int N = terra_N(); int16_t* hm = terra_hm();
        float vs = *(const float*)0x022F227C;
        float hs = *(const float*)0x022F2278;
        float yo = *(const float*)0x022F2274;
        if (N <= 0 || !hm || vs <= 0.0f) return 0.0f;

        float fcx = worldX / vs, fcz = worldZ / vs;
        int cx0 = (int)fcx, cz0 = (int)fcz;
        int cx1 = cx0+1,   cz1 = cz0+1;
        cx0 = max(0,min(N-1,cx0)); cz0 = max(0,min(N-1,cz0));
        cx1 = max(0,min(N-1,cx1)); cz1 = max(0,min(N-1,cz1));
        float tx = fcx-(int)fcx, tz = fcz-(int)fcz;

        float h00=(float)(uint16_t)hm[(size_t)cz0*N+cx0];
        float h10=(float)(uint16_t)hm[(size_t)cz0*N+cx1];
        float h01=(float)(uint16_t)hm[(size_t)cz1*N+cx0];
        float h11=(float)(uint16_t)hm[(size_t)cz1*N+cx1];
        float hv=h00*(1-tx)*(1-tz)+h10*tx*(1-tz)+h01*(1-tx)*tz+h11*tx*tz;
        return hv*hs+yo;
    }

    static const ImU32 kTerraTypeColors[] = {
        IM_COL32( 90,140, 70,255), IM_COL32(170,145, 90,255),
        IM_COL32(130,125,120,255), IM_COL32(220,200,145,255),
        IM_COL32(230,240,255,255), IM_COL32( 55,115,185,255),
        IM_COL32(200, 50, 30,255), IM_COL32( 65, 80, 60,255),
    };
    static const int kTerraNumTypes = 8;

    static void terra_release_tex()
    {
        if (s_terraCanvasTex) { s_terraCanvasTex->Release(); s_terraCanvasTex = nullptr; }
        if (s_terraMiniTex)   { s_terraMiniTex->Release();   s_terraMiniTex   = nullptr; }
        s_terraMiniMapId   = 0xFFFF;
        s_terraWldMapId    = 0xFFFF;
        s_terraTypeCount   = 0;
        s_terraCanvasDirty = true;
        // Reset undo state (la mappa potrebbe cambiare)
        s_undoStack.clear();
        s_strokeOpen = false;
        s_hmRecorded.clear();
        s_tmRecorded.clear();
        s_terraOrigHM.clear();
        s_terraOrigTM.clear();
        s_terraOrigN = 0;
        s_miniZoom = 1.0f;
        s_miniPanX = 0.0f;
        s_miniPanZ = 0.0f;
    }

    static void terra_refresh_tex(bool forceType = false)
    {
        IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x022b69a8;
        if (!dev) return;
        if (!s_terraCanvasTex) {
            if (FAILED(dev->CreateTexture(256, 256, 1, D3DUSAGE_DYNAMIC,
                    D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_terraCanvasTex, nullptr)))
                return;
        }
        int N = terra_N();
        int16_t* hm = terra_hm();
        uint8_t* tm = terra_tm();
        if (N <= 0 || !hm) return;

        D3DLOCKED_RECT lr{};
        if (FAILED(s_terraCanvasTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) return;
        uint32_t* pix = (uint32_t*)lr.pBits;
        int pitch = (int)(lr.Pitch / 4);
        for (int cy = 0; cy < 256; cy++) {
            for (int cx2 = 0; cx2 < 256; cx2++) {
                int hx = cx2 * N / 256, hy = cy * N / 256;
                int idx = hy * N + hx;
                uint32_t col;
                if ((s_terraShowTypes && tm && s_terraBrushMode == 3) || (forceType && tm)) {
                    col = kTerraTypeColors[tm[idx] % kTerraNumTypes];
                    // checkerboard luminance: pixel alternati schiariti (alzato) o scuriti (abbassato)
                    // color-neutral: contrasta con qualsiasi colore tipo senza conflitti
                    if (!s_terraOrigHM.empty() && s_terraOrigN == N) {
                        int32_t delta = (int32_t)hm[idx] - (int32_t)s_terraOrigHM[idx];
                        if (delta != 0 && ((cx2 + cy) & 1)) {
                            uint32_t r = (col >> 16) & 0xFF;
                            uint32_t g = (col >>  8) & 0xFF;
                            uint32_t b =  col        & 0xFF;
                            if (delta > 0) {
                                // alzato → schiarisci verso bianco (55% white)
                                r = r * 45/100 + 140;
                                g = g * 45/100 + 140;
                                b = b * 45/100 + 140;
                            } else {
                                // abbassato → scurisci verso nero (55% black)
                                r = r * 45/100;
                                g = g * 45/100;
                                b = b * 45/100;
                            }
                            col = 0xFF000000u | (r << 16) | (g << 8) | b;
                        }
                    }
                } else if (!s_terraOrigHM.empty() && s_terraOrigN == N) {
                    // delta dall'originale: transparent=invariato, arancione=alzato, ciano=abbassato
                    int32_t delta = (int32_t)hm[idx] - (int32_t)s_terraOrigHM[idx];
                    if (delta == 0) {
                        col = 0x00000000u;
                    } else if (delta > 0) {
                        uint8_t mag = (uint8_t)min(255, 160 + (delta >> 2));
                        col = 0xE0000000u | ((uint32_t)mag << 16) | ((uint32_t)(mag / 2) << 8);
                    } else {
                        uint8_t mag = (uint8_t)min(255, 160 + ((-delta) >> 2));
                        col = 0xE0000000u | ((uint32_t)(mag / 2) << 8) | (uint32_t)mag;
                    }
                } else {
                    // nessun originale: grayscale tenue (orienta l'utente prima del primo edit)
                    uint8_t lum = (uint8_t)(((int)hm[idx] - (-32768)) >> 8);
                    col = 0x50000000u | ((uint32_t)lum<<16) | ((uint32_t)lum<<8) | lum;
                }
                pix[cy * pitch + cx2] = col;
            }
        }
        s_terraCanvasTex->UnlockRect(0);
        s_terraCanvasDirty = false;
    }

    // worldSize reale dal WLD header (CWorldMgr+0x140254 = 0x904CBC)
    static float terra_world_size()
    {
        float ws = *(float*)0x00904CBC;
        if (ws > 0.0f && ws < 1e7f) return ws;
        // fallback: N * vertSpacing
        int N = terra_N();
        float vs = *(float*)0x022F227C;
        return (N > 0 && vs > 0.0f) ? (float)N * vs : (float)N;
    }

    // ── Undo helpers ────────────────────────────────────────────────
    static void terra_save_original()
    {
        if (s_terraOrigN > 0) return;
        int N = terra_N();
        if (N <= 0) return;
        int16_t* hm = terra_hm();
        uint8_t* tm = terra_tm();
        if (!hm) return;
        s_terraOrigHM.assign(hm, hm + N*N);
        if (tm) s_terraOrigTM.assign(tm, tm + N*N);
        s_terraOrigN = N;
    }

    static void terra_undo_begin()
    {
        if (s_strokeOpen) return;
        terra_save_original();
        s_undoStack.emplace_back();
        if ((int)s_undoStack.size() > kMaxUndo)
            s_undoStack.erase(s_undoStack.begin());
        s_strokeOpen = true;
        s_hmRecorded.clear();
        s_tmRecorded.clear();
    }

    static void terra_undo_end()
    {
        if (!s_strokeOpen) return;
        s_strokeOpen = false;
        if (s_undoStack.back().hmOld.empty() && s_undoStack.back().tmOld.empty())
            s_undoStack.pop_back();
        s_hmRecorded.clear();
        s_tmRecorded.clear();
    }

    static void terra_undo_record_hm(int idx, int16_t old_v)
    {
        if (!s_strokeOpen || s_hmRecorded.count(idx)) return;
        s_hmRecorded.insert(idx);
        s_undoStack.back().hmOld.push_back({idx, old_v});
    }

    static void terra_undo_record_tm(int idx, uint8_t old_v)
    {
        if (!s_strokeOpen || s_tmRecorded.count(idx)) return;
        s_tmRecorded.insert(idx);
        s_undoStack.back().tmOld.push_back({idx, old_v});
    }

    static void terra_undo()
    {
        if (s_undoStack.empty()) return;
        auto& stroke = s_undoStack.back();
        int16_t* hm = terra_hm();
        uint8_t* tm = terra_tm();
        if (hm) for (auto& [idx,v] : stroke.hmOld) hm[idx] = v;
        if (tm) for (auto& [idx,v] : stroke.tmOld) tm[idx] = v;
        s_undoStack.pop_back();
        s_terraHasEdits    = !s_undoStack.empty();
        s_terraCanvasDirty = true;
        snprintf(s_terraMsg, sizeof(s_terraMsg), "Annullata ultima modifica. Undo rimasti: %d", (int)s_undoStack.size());
    }

    static void terra_reset_all()
    {
        if (s_terraOrigN <= 0) { snprintf(s_terraMsg,sizeof(s_terraMsg),"Nessuna modifica da annullare."); return; }
        int N = terra_N();
        if (N != s_terraOrigN) { snprintf(s_terraMsg,sizeof(s_terraMsg),"Errore: N mismatch nel reset."); return; }
        int16_t* hm = terra_hm();
        uint8_t* tm = terra_tm();
        if (hm && !s_terraOrigHM.empty()) memcpy(hm, s_terraOrigHM.data(), N*N*sizeof(int16_t));
        if (tm && !s_terraOrigTM.empty()) memcpy(tm, s_terraOrigTM.data(), N*N*sizeof(uint8_t));
        s_undoStack.clear();
        s_terraHasEdits    = false;
        s_terraCanvasDirty = true;
        snprintf(s_terraMsg, sizeof(s_terraMsg), "Tutte le modifiche annullate (reset completo).");
    }

    static void terra_apply_brush(int centerX, int centerZ)
    {
        int N = terra_N();
        if (N <= 0) return;
        int16_t* hm = terra_hm();
        uint8_t* tm = terra_tm();
        if (!hm) return;
        int R = (s_terraBrushR < 1.0f) ? 1 : (int)s_terraBrushR;
        for (int dz = -R; dz <= R; dz++) {
            for (int dx = -R; dx <= R; dx++) {
                float d = sqrtf((float)(dx*dx + dz*dz)) / (float)R;
                if (d > 1.0f) continue;
                float fo = 1.0f - d;
                int px2 = centerX + dx, pz2 = centerZ + dz;
                if (px2 < 0 || px2 >= N || pz2 < 0 || pz2 >= N) continue;
                int idx = pz2 * N + px2;
                switch (s_terraBrushMode) {
                case 0: {
                    terra_undo_record_hm(idx, hm[idx]);
                    float v = (float)hm[idx] + s_terraBrushStr * fo;
                    if (v >  32767.0f) v =  32767.0f;
                    if (v < -32768.0f) v = -32768.0f;
                    hm[idx] = (int16_t)v;
                    break;
                }
                case 1: {
                    terra_undo_record_hm(idx, hm[idx]);
                    float v = (float)hm[idx] - s_terraBrushStr * fo;
                    if (v >  32767.0f) v =  32767.0f;
                    if (v < -32768.0f) v = -32768.0f;
                    hm[idx] = (int16_t)v;
                    break;
                }
                case 2: {
                    int sum = 0, cnt = 0;
                    int nx[4] = {px2-1, px2+1, px2,   px2  };
                    int nz[4] = {pz2,   pz2,   pz2-1, pz2+1};
                    for (int k = 0; k < 4; k++) {
                        if (nx[k]>=0&&nx[k]<N&&nz[k]>=0&&nz[k]<N)
                            { sum += hm[nz[k]*N+nx[k]]; cnt++; }
                    }
                    if (cnt > 0) {
                        terra_undo_record_hm(idx, hm[idx]);
                        float avg = (float)sum / cnt;
                        float v = hm[idx] + (avg - hm[idx]) * 0.25f * fo;
                        if (v >  32767.0f) v =  32767.0f;
                        if (v < -32768.0f) v = -32768.0f;
                        hm[idx] = (int16_t)v;
                    }
                    break;
                }
                case 3:
                    if (tm) {
                        terra_undo_record_tm(idx, tm[idx]);
                        tm[idx] = (uint8_t)s_terraPaintType;
                    }
                    break;
                }
            }
        }
        s_terraHasEdits    = true;
        s_terraCanvasDirty = true;
    }

    static void terra_save_wld()
    {
        int N = terra_N();
        if (N <= 0) { snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: N=%d (terrain non caricato).",N); return; }
        int16_t* hm = terra_hm();
        if (!hm) { snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: heightMap null."); return; }

        uint16_t mapId = get_current_map_id();
        if (mapId == 0xFFFF) { snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: non loggato (CPlayerData null)."); return; }

        char dllDir[MAX_PATH] = {};
        if (!get_dll_dir(dllDir,sizeof(dllDir))) { snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: path DLL."); return; }
        char src[MAX_PATH], dst[MAX_PATH];
        snprintf(src,MAX_PATH,"%s\\Data\\World\\%u.wld",dllDir,(unsigned)mapId);
        snprintf(dst,MAX_PATH,"%s\\Output_Maps\\World\\%u.wld",dllDir,(unsigned)mapId);

        FILE* f = nullptr;
        fopen_s(&f, src, "rb");
        if (!f) { snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: '%s' non trovato.",src); return; }
        fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
        size_t minSz = 8 + (size_t)N*N*3;
        if (fsz < (long)minSz) { fclose(f); snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: WLD troppo piccolo (fsz=%ld < %zu).",fsz,minSz); return; }
        std::vector<uint8_t> buf((size_t)fsz);
        fread(buf.data(),1,(size_t)fsz,f); fclose(f);

        // Patch height map (offset 8, N*N*2 bytes)
        memcpy(buf.data()+8, hm, (size_t)N*N*2);
        // Patch type map (offset 8+N*N*2, N*N bytes)
        uint8_t* tm = terra_tm();
        if (tm) memcpy(buf.data()+8+(size_t)N*N*2, tm, (size_t)N*N);

        ensure_dir(dst);
        FILE* out = nullptr;
        fopen_s(&out, dst, "wb");
        if (!out) { snprintf(s_terraMsg,sizeof(s_terraMsg),"ERRORE: impossibile scrivere '%s'.",dst); return; }
        fwrite(buf.data(),1,(size_t)fsz,out); fclose(out);
        snprintf(s_terraMsg,sizeof(s_terraMsg),"Salvato: Output_Maps\\World\\%u.wld  (N=%d, heightmap+typemap patchati)",(unsigned)mapId,N);
        s_terraHasEdits = false;
    }

    // Carica minimap PNG dal gioco (Data\Interface\MiniMap\{mapId}.png)
    static void terra_load_minimap(uint16_t mapId)
    {
        if (s_terraMiniTex && s_terraMiniMapId == mapId) return; // già caricata
        if (s_terraMiniTex) { s_terraMiniTex->Release(); s_terraMiniTex = nullptr; }
        s_terraMiniW = s_terraMiniH = 0;
        s_terraMiniMapId = mapId;

        char dllDir[MAX_PATH] = {};
        if (!get_dll_dir(dllDir, sizeof(dllDir))) return;
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\Data\\Interface\\MiniMap\\%u.png", dllDir, (unsigned)mapId);

        FILE* f = nullptr; fopen_s(&f, path, "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
        if (fsz <= 0) { fclose(f); return; }
        std::vector<uint8_t> buf((size_t)fsz);
        fread(buf.data(), 1, (size_t)fsz, f); fclose(f);

        int w = 0, h = 0, ch = 0;
        stbi_uc* px = stbi_load_from_memory(buf.data(), (int)fsz, &w, &h, &ch, 4);
        if (!px) return;

        IDirect3DDevice9* dev = *(IDirect3DDevice9**)0x022b69a8;
        if (!dev) { stbi_image_free(px); return; }

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(dev->CreateTexture((UINT)w, (UINT)h, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
            stbi_image_free(px); return;
        }
        D3DLOCKED_RECT lr{};
        if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
            tex->Release(); stbi_image_free(px); return;
        }
        for (int y = 0; y < h; y++) {
            stbi_uc* src = px + y * w * 4;
            BYTE*    dst = (BYTE*)lr.pBits + y * lr.Pitch;
            for (int x = 0; x < w; x++) {
                dst[x*4+0] = src[x*4+2]; // B
                dst[x*4+1] = src[x*4+1]; // G
                dst[x*4+2] = src[x*4+0]; // R
                dst[x*4+3] = src[x*4+3]; // A
            }
        }
        tex->UnlockRect(0);
        stbi_image_free(px);
        s_terraMiniTex   = tex;
        s_terraMiniW     = w;
        s_terraMiniH     = h;
    }

    // Legge nomi texture terreno dalla sezione terrain del WLD
    static void terra_load_type_names(uint16_t mapId)
    {
        if (s_terraWldMapId == mapId) return;
        s_terraWldMapId  = mapId;
        s_terraTypeCount = 0;
        memset(s_terraTypeNames, 0, sizeof(s_terraTypeNames));

        int N = terra_N();
        if (N <= 0) return;
        char dllDir[MAX_PATH] = {};
        if (!get_dll_dir(dllDir, sizeof(dllDir))) return;
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\Data\\World\\%u.wld", dllDir, (unsigned)mapId);
        FILE* f = nullptr; fopen_s(&f, path, "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END); long fsz = ftell(f); rewind(f);
        if (fsz < 8 + (long)N*N*3 + 4) { fclose(f); return; }

        fseek(f, (long)(8 + (size_t)N*N*3), SEEK_SET);
        int32_t cnt = 0;
        fread(&cnt, 4, 1, f);
        if (cnt < 0 || cnt > 16) cnt = 0;
        s_terraTypeCount = cnt;
        for (int i = 0; i < cnt; i++) {
            char brushName[256] = {};
            fread(brushName, 256, 1, f);
            // Tronca a senza path, solo filename
            const char* base = strrchr(brushName, '\\');
            if (!base) base = strrchr(brushName, '/');
            if (base) base++; else base = brushName;
            strncpy_s(s_terraTypeNames[i], sizeof(s_terraTypeNames[i]), base, _TRUNCATE);
            // Rimuovi estensione .dds
            char* dot = strrchr(s_terraTypeNames[i], '.');
            if (dot) *dot = '\0';
            float tileSize = 0.0f; fread(&tileSize, 4, 1, f);
            char detailName[256] = {}; fread(detailName, 256, 1, f);
        }
        fclose(f);
    }

    // ── Alpha DDS export ────────────────────────────────────────────
    // Legge le DDS originali DXT3 da srcDir (256×256, 9 mip, 87408 byte payload),
    // fa overlay SOLO sui DXT3 block delle celle dipinte, scrive in outDir.
    // Nome file: {mapId}_{row}{col}_A{T}.dds  (es: 2_00_A0.dds)
    static void terra_export_alpha_dds(const char* outDir, const char* srcDir, uint16_t mapId)
    {
        int N = terra_N();
        uint8_t* tm = terra_tm();
        if (N <= 0 || !tm) {
            snprintf(s_terraMsg, sizeof(s_terraMsg), "Alpha DDS: terrain non caricato."); return;
        }
        int numTypes = (s_terraTypeCount > 0) ? s_terraTypeCount : kTerraNumTypes;
        int M = *(int*)(0x7C4A74 + 4);
        if (M <= 0 || M > 16) M = 1;
        int cellsPerPatch = (N - 1) / M;
        if (cellsPerPatch <= 0) cellsPerPatch = N / M;

        struct DDSHdr {
            uint32_t magic, size, flags, height, width, linearSize, depth, mipMapCount;
            uint32_t reserved1[11];
            uint32_t pfSize, pfFlags, pfFourCC, pfBitCount, pfRMask, pfGMask, pfBMask, pfAMask;
            uint32_t caps, caps2, caps3, caps4, reserved2;
        };
        static_assert(sizeof(DDSHdr) == 128, "DDS header size");

        // Costanti DXT3 verificate dai file originali (256×256, 9 mip, DXT3)
        // DXT3: 16 byte per blocco 4×4. Byte 0-7 = alpha nibbles, Byte 8-15 = color DXT1.
        // Mip 0 size: (256/4)*(256/4)*16 = 64*64*16 = 65536 byte
        // Total mip chain: 65536+16384+4096+1024+256+64+16+16+16 = 87408 byte
        const int kTexW          = 256;
        const int kTexH          = 256;
        const int kNumMips       = 9;
        const int kBlocksX       = kTexW / 4;          // 64
        const int kBlocksY       = kTexH / 4;          // 64
        const int kMip0Size      = kBlocksX * kBlocksY * 16; // 65536
        const int kTotalDXTSize  = 87408;

        char mapStr[16];
        snprintf(mapStr, sizeof(mapStr), "%u", (unsigned)mapId);
        int exported = 0, skipped = 0;

        for (int pr = 0; pr < M; pr++) {
            for (int pc = 0; pc < M; pc++) {
                int baseRow = pr * cellsPerPatch;
                int baseCol = pc * cellsPerPatch;

                for (int t = 0; t < numTypes; t++) {
                    // Early-out: nessuna cella dipinta in questo patch?
                    bool anyChange = false;
                    for (int cy = 0; cy < cellsPerPatch && !anyChange; cy++) {
                        int gy = baseRow + cy; if (gy >= N) break;
                        for (int cx = 0; cx < cellsPerPatch && !anyChange; cx++) {
                            int gx = baseCol + cx; if (gx >= N) break;
                            size_t idx = (size_t)gy * N + gx;
                            if (s_terraOrigTM.empty() || tm[idx] != s_terraOrigTM[idx])
                                anyChange = true;
                        }
                    }
                    if (!anyChange) { skipped++; continue; }

                    // Path output (calcolato prima per usarlo come sorgente preferita)
                    char outPath[512];
                    snprintf(outPath, sizeof(outPath), "%s\\%s_%d%d_A%d.dds",
                        outDir, mapStr, pr, pc, t);
                    // Sorgente originale (fallback)
                    char srcPath[512];
                    snprintf(srcPath, sizeof(srcPath), "%s\\%s_%d%d_A%d.dds",
                        srcDir, mapStr, pr, pc, t);

                    DDSHdr hdr = {};
                    std::vector<uint8_t> dxt3(kTotalDXTSize, 0u);

                    // Preferisci il file già esportato (mantiene modifiche sessioni precedenti)
                    FILE* fsrc = nullptr;
                    fopen_s(&fsrc, outPath, "rb");
                    if (!fsrc) fopen_s(&fsrc, srcPath, "rb");
                    if (fsrc) {
                        if (fread(&hdr, 1, sizeof(hdr), fsrc) == sizeof(hdr)
                            && hdr.magic == 0x20534444u) {
                            fread(dxt3.data(), 1, kTotalDXTSize, fsrc);
                        } else {
                            hdr = {};
                        }
                        fclose(fsrc);
                    }

                    if (hdr.magic != 0x20534444u) {
                        // Genera header DXT3 da zero (originale non trovato)
                        hdr = {};
                        hdr.magic       = 0x20534444u;
                        hdr.size        = 124;
                        hdr.flags       = 0x00021007u; // CAPS|HEIGHT|WIDTH|LINEARSIZE|PIXELFORMAT|MIPMAPCOUNT
                        hdr.height      = (uint32_t)kTexH;
                        hdr.width       = (uint32_t)kTexW;
                        hdr.linearSize  = (uint32_t)kMip0Size;
                        hdr.mipMapCount = (uint32_t)kNumMips;
                        hdr.pfSize      = 32;
                        hdr.pfFlags     = 0x00000004u; // DDPF_FOURCC
                        hdr.pfFourCC    = 0x33545844u; // "DXT3"
                        hdr.caps        = 0x00401008u; // COMPLEX|MIPMAP|TEXTURE
                        // dxt3: alpha=0 (trasparente), color=grigio neutro (0x8410)
                        // 0x8410 = valore medio osservato negli originali mappa 2
                        for (int b = 0; b < kTotalDXTSize; b += 16) {
                            dxt3[b+8] = 0x10; dxt3[b+9] = 0x84; // c0 = 0x8410 (LE)
                            dxt3[b+10] = 0x10; dxt3[b+11] = 0x84; // c1 = 0x8410
                            // byte 0-7 = 0 (alpha = 0), byte 12-15 = 0 (indices → c0)
                        }
                    }

                    // Loop BLOCK-centric: evita il clobber del vecchio approccio cell-centric.
                    // Precedentemente: per ogni cella cambiata si sovrascriveva l'intero blocco
                    // DXT3 con alpha uniforme → celle vicine non cambiate venivano corrotte →
                    // terreno trasparente/scomparso a R>10, e R=1..10 visivamente identici.
                    //
                    // Nuovo approccio: per ogni blocco DXT3, se almeno una cella coperta è
                    // cambiata → ricalcola i 16 nibble alpha uno per uno.
                    // Ogni pixel del blocco riceve alpha = frazione delle celle che copre con tipo t.
                    // Risultato: per-pixel alpha, bordi sfumati naturali, nessun clobber.
                    for (int bby = 0; bby < kBlocksY; bby++) {
                        for (int bbx = 0; bbx < kBlocksX; bbx++) {
                            // Range celle coperte da questo blocco (stima larga per early-out)
                            int cxMin = (bbx * 4) * cellsPerPatch / kTexW;
                            int cxMax = ((bbx + 1) * 4) * cellsPerPatch / kTexW + 1;
                            int cyMin = (bby * 4) * cellsPerPatch / kTexH;
                            int cyMax = ((bby + 1) * 4) * cellsPerPatch / kTexH + 1;

                            bool blockChanged = false;
                            for (int cry = cyMin; cry < cyMax && !blockChanged; cry++) {
                                int gy = baseRow + cry; if (gy >= N) break;
                                for (int crx = cxMin; crx < cxMax && !blockChanged; crx++) {
                                    int gx = baseCol + crx; if (gx >= N) break;
                                    size_t idx = (size_t)gy * N + gx;
                                    if (s_terraOrigTM.empty() || tm[idx] != s_terraOrigTM[idx])
                                        blockChanged = true;
                                }
                            }
                            if (!blockChanged) continue; // mantieni alpha originale dal DDS caricato

                            // Ricalcola i 16 nibble alpha (byte 0-7 del blocco DXT3).
                            // Color block (byte 8-15) rimane dall'originale.
                            size_t blkOff = ((size_t)bby * kBlocksX + bbx) * 16;
                            uint8_t alphaBytes[8] = {};
                            for (int prow = 0; prow < 4; prow++) {
                                for (int pcol = 0; pcol < 4; pcol++) {
                                    int pixX = bbx * 4 + pcol;
                                    int pixY = bby * 4 + prow;
                                    // Celle coperte da questo pixel (può essere >1 se texture < cellsPerPatch)
                                    int cx0 = pixX * cellsPerPatch / kTexW;
                                    int cx1 = (pixX + 1) * cellsPerPatch / kTexW;
                                    int cy0 = pixY * cellsPerPatch / kTexH;
                                    int cy1 = (pixY + 1) * cellsPerPatch / kTexH;
                                    if (cx1 <= cx0) cx1 = cx0 + 1;
                                    if (cy1 <= cy0) cy1 = cy0 + 1;
                                    // alpha = celle con tipo t / totale celle coperte (0..15)
                                    int total = 0, typeCount = 0;
                                    for (int cry2 = cy0; cry2 < cy1; cry2++) {
                                        int gy = baseRow + cry2; if (gy >= N) continue;
                                        for (int crx2 = cx0; crx2 < cx1; crx2++) {
                                            int gx = baseCol + crx2; if (gx >= N) continue;
                                            total++;
                                            if (tm[(size_t)gy * N + gx] == (uint8_t)t) typeCount++;
                                        }
                                    }
                                    uint8_t a4 = total > 0
                                        ? (uint8_t)(typeCount * 15 / total)
                                        : 0u;
                                    // Nibble packing: nibble_idx = prow*4+pcol,
                                    // byte = nibble_idx/2, low nibble se pari, high se dispari.
                                    int nibbleIdx = prow * 4 + pcol;
                                    if (nibbleIdx % 2 == 0)
                                        alphaBytes[nibbleIdx / 2] = (alphaBytes[nibbleIdx / 2] & 0xF0u) | a4;
                                    else
                                        alphaBytes[nibbleIdx / 2] = (alphaBytes[nibbleIdx / 2] & 0x0Fu) | (uint8_t)(a4 << 4);
                                }
                            }
                            memcpy(&dxt3[blkOff], alphaBytes, 8);
                        }
                    }

                    // Scrivi DDS completo (header + tutti i mip, 87536 byte totali)
                    FILE* fout = nullptr;
                    fopen_s(&fout, outPath, "wb");
                    if (!fout) continue;
                    fwrite(&hdr, sizeof(hdr), 1, fout);
                    fwrite(dxt3.data(), 1, kTotalDXTSize, fout);
                    fclose(fout);
                    exported++;
                }
            }
        }
        snprintf(s_terraMsg, sizeof(s_terraMsg),
            "Alpha DXT3: %d modificati, %d invariati. "
            "Copia in Data\\World\\Field\\%s\\ e ricarica mappa.",
            exported, skipped, mapStr);
    }

    // Rebuild terrain in-memory → 3D (chiama FUN_005dae90 per patch + FUN_005c90d0)
    // Funziona per modifiche di altezza. Tipi richiedono save+reload.
    static void terra_rebuild_live()
    {
        int N = terra_N();
        int M = (*(int*)(0x7C4A74 + 4));
        if (N <= 0 || M <= 0 || M > 32) {
            snprintf(s_terraMsg, sizeof(s_terraMsg), "ERRORE rebuild: N=%d M=%d", N, M); return;
        }
        int vertsPerPatch = *(int*)0x022F228C;
        float vertSpacing = *(float*)0x022F227C;
        if (vertsPerPatch < 2) { snprintf(s_terraMsg, sizeof(s_terraMsg), "ERRORE: vertsPerPatch=%d", vertsPerPatch); return; }
        float patchWorldSz = vertSpacing * (float)(vertsPerPatch - 1);

        typedef void (__thiscall* RebuildPatch_t)(void*, float, float);
        static const RebuildPatch_t s_rebuildPatch = (RebuildPatch_t)0x005dae90;
        typedef void (__fastcall* FinalizeAll_t)(void*);
        static const FinalizeAll_t  s_finalizeAll  = (FinalizeAll_t) 0x005c90d0;

        int rebuilt = 0;
        for (int pr = 0; pr < M; pr++) {
            for (int pc = 0; pc < M; pc++) {
                int   patchIdx = M * pr + pc;
                void* patchPtr = (void*)(0x7C4A74 + 8 + (size_t)patchIdx * 0x4F308);
                float worldX   = patchWorldSz * (float)pc;
                float worldZ   = patchWorldSz * (float)pr;
                __try { s_rebuildPatch(patchPtr, worldX, worldZ); rebuilt++; }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        __try { s_finalizeAll((void*)0x7C4A74); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            snprintf(s_terraMsg, sizeof(s_terraMsg), "Rebuild: %d patch ok, finalize crash.", rebuilt); return;
        }

        // FUN_005db590 legge subpatch+0x70 (typeMap ORIGINALE del LoadWorld, non aggiornato).
        // Celle appena dipinte con tipo non-zero → flag quel tipo resta 0 → trasparenza.
        // Fix: tutti e 8 i flag texture (subpatch+0xd0..0xd7) = 1 → renderer lega tutti i layer;
        // per ogni cella il vertex type seleziona il peso giusto → colore corretto, no trasparenza.
        int subPerSide = *(int*)0x022f2288;
        int fixedSubs  = 0;
        if (subPerSide > 0 && subPerSide < 256) {
            int numSub = subPerSide * subPerSide;
            for (int pr2 = 0; pr2 < M; pr2++) {
                for (int pc2 = 0; pc2 < M; pc2++) {
                    int   pIdx  = M * pr2 + pc2;
                    auto* pBase = (uint8_t*)(0x7C4A74 + 8) + (size_t)pIdx * 0x4F308;
                    for (int sp = 0; sp < numSub; sp++) {
                        memset(pBase + 300 + sp * (0x4e * 4) + 0xd0, 1, 8);
                        fixedSubs++;
                    }
                }
            }
        }
        snprintf(s_terraMsg, sizeof(s_terraMsg),
            "Rebuild live: %d patch, sps=%d, %d sub fissati.", rebuilt, subPerSide, fixedSubs);
    }

    static void draw_terra_content()
    {
        int N = terra_N();
        int16_t* hm = terra_hm();
        uint8_t* tm = terra_tm();

        if (N <= 0 || !hm) {
            ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1.0f), "Terrain non caricato (N=%d, hm=%s)", N, hm?"ok":"null");
            ImGui::TextDisabled("Entra in una zona di gioco per caricare il terrain.");
            return;
        }

        // Carica minimap e type names se la mappa e' cambiata
        uint16_t mapId = g_pPlayerData ? g_pPlayerData->mapId : (uint16_t)0xFFFF;
        if (mapId != 0xFFFF) {
            terra_load_minimap(mapId);
            terra_load_type_names(mapId);
        }

        // Status bar
        ImGui::TextColored(ImVec4(0.4f,1.0f,0.5f,1.0f), "Terrain OK  N=%d", N);
        ImGui::SameLine();
        ImGui::TextDisabled("| Mappa %u | Patch M=%d", (unsigned)mapId, *(int*)(0x7C4A74+4));
        if (s_terraHasEdits) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f,0.9f,0.2f,1.0f), "[MODIFICATO]"); }
        ImGui::Separator();

        // ── Modalita' pennello ──
        ImGui::Text("Pennello:");
        static const char* kML[] = { "Alza", "Abbassa", "Smooth", "Tipo" };
        for (int i = 0; i < 4; i++) {
            ImGui::SameLine();
            bool sel = (s_terraBrushMode == i);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f,0.55f,0.25f,1.0f));
            if (ImGui::SmallButton(kML[i])) s_terraBrushMode = i;
            if (sel) ImGui::PopStyleColor();
        }

        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat("Raggio (celle)", &s_terraBrushR, 1.0f, 50.0f, "%.0f");
        if (s_terraBrushMode < 3) {
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat("Forza (int16/frame)", &s_terraBrushStr, 10.0f, 3000.0f, "%.0f");
        } else {
            // Tipo: mostra nomi reali dal WLD se disponibili
            int numTypes = (s_terraTypeCount > 0) ? s_terraTypeCount : kTerraNumTypes;
            for (int i = 0; i < numTypes; i++) {
                if (i > 0) ImGui::SameLine(0.0f, 3.0f);
                bool sel = (s_terraPaintType == i);
                ImVec4 c = ImGui::ColorConvertU32ToFloat4(kTerraTypeColors[i % kTerraNumTypes]);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    sel ? c : ImVec4(c.x*.55f, c.y*.55f, c.z*.55f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
                char lbl[8]; snprintf(lbl, 8, "##tt%d", i);
                if (ImGui::Button(lbl, ImVec2(22.0f, 22.0f))) s_terraPaintType = i;
                ImGui::PopStyleColor(2);
                const char* tipName = (s_terraTypeCount > 0 && i < s_terraTypeCount)
                    ? s_terraTypeNames[i] : "???";
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("[%d] %s", i, tipName);
            }
            if (s_terraTypeCount > 0 && s_terraPaintType < s_terraTypeCount)
                ImGui::TextDisabled("  %s", s_terraTypeNames[s_terraPaintType]);
            ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.0f),
                "Dipingi → Esporta Alpha DDS → copia in data\\Field\\{mapId}\\ → inserisci SAH → ricarica mappa.");
        }

        ImGui::Separator();

        // Ctrl+Z = undo
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            terra_undo();

        // ── Vista: minimap di gioco (principale) + canvas tipo (in modo Tipo) ──
        float avail = ImGui::GetContentRegionAvail().x - 4.0f;
        float sz    = avail < 512.0f ? avail : 512.0f;
        float vs    = *(float*)0x022F227C;  // world units per vertex

        // worldSz: usa N*vertSpacing (formula diretta dal terrain runtime)
        float worldSz = (N > 0 && vs > 0.5f && vs < 1000.0f)
            ? (float)N * vs
            : terra_world_size();

        // Player world position (CCharacter* @ 0x904AA0, pos @ +0x10/+0x18)
        float playerX = -99999.0f, playerZ = -99999.0f;
        {
            void* pUser = *(void**)0x904AA0;
            if (pUser && (uintptr_t)pUser > 0xFFFFu) {
                playerX = *(float*)((uint8_t*)pUser + 0x10);
                playerZ = *(float*)((uint8_t*)pUser + 0x18);
            }
        }

        // Debug + controlli zoom
        ImGui::TextDisabled("N=%d vs=%.2f worldSz=%.0f  px=(%.0f,%.0f)",
            N, vs, worldSz, playerX, playerZ);
        ImGui::SameLine();
        ImGui::Text("Zoom:%.1fx", s_miniZoom);
        ImGui::SameLine();
        if (ImGui::SmallButton("1:1##zr")) { s_miniZoom = 1.0f; s_miniPanX = 0.0f; s_miniPanZ = 0.0f; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reimposta zoom (1:1)");
        ImGui::SameLine();
        ImGui::TextDisabled("Scroll=zoom  Drag(R)=sposta");

        // Marca canvas dirty quando l'utente cambia modo (height↔tipo)
        {
            static int s_prevBrushMode = -1;
            if (s_terraBrushMode != s_prevBrushMode) {
                s_prevBrushMode    = s_terraBrushMode;
                s_terraCanvasDirty = true;
            }
        }

        // In modo Tipo: usa canvas con colori tipo per feedback visivo immediato
        // (minimap statica non mostra le modifiche tipo prima di Rebuild Live)
        bool useTipoCanvas = (s_terraBrushMode == 3);
        if (useTipoCanvas) s_terraShowTypes = true;

        IDirect3DTexture9* displayTex = nullptr;
        bool useMini = false;
        if (useTipoCanvas || !s_terraMiniTex) {
            if (s_terraCanvasDirty) terra_refresh_tex();
            displayTex = s_terraCanvasTex;
            useMini    = false;
        } else {
            displayTex = s_terraMiniTex;
            useMini    = true;
        }

        // Traccia stato mouse per undo stroke
        static bool s_wasPainting = false;
        bool isPainting = false;

        if (displayTex) {
            // UV di zoom: mostra la regione [panX, panX+viewSzW] x [panZ, panZ+viewSzW]
            float viewSzW = (worldSz > 0.0f) ? worldSz / s_miniZoom : worldSz;
            float ux0 = (worldSz > 0.0f) ? s_miniPanX / worldSz : 0.0f;
            float ux1 = ux0 + (worldSz > 0.0f ? viewSzW / worldSz : 1.0f);
            ImVec2 uvMin, uvMax;
            if (!useMini) {
                // Canvas: riga 0=Z=0=sud; flip Y → top display=nord=Z grande
                float uy_top    = (worldSz > 0.0f) ? (s_miniPanZ + viewSzW) / worldSz : 1.0f;
                float uy_bottom = (worldSz > 0.0f) ?  s_miniPanZ             / worldSz : 0.0f;
                uvMin = ImVec2(ux0, uy_top);
                uvMax = ImVec2(ux1, uy_bottom);
            } else {
                // Minimap PNG: riga 0=nord=Z grande; Y normale
                float uy_top    = (worldSz > 0.0f) ? 1.0f - (s_miniPanZ + viewSzW) / worldSz : 0.0f;
                float uy_bottom = (worldSz > 0.0f) ? 1.0f -  s_miniPanZ             / worldSz : 1.0f;
                uvMin = ImVec2(ux0, uy_top);
                uvMax = ImVec2(ux1, uy_bottom);
            }

            ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)displayTex, ImVec2(sz, sz), uvMin, uvMax);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Overlay delta heightmap sulla minimap (sempre in height mode)
            if (useMini && s_terraBrushMode != 3) {
                if (s_terraCanvasDirty) terra_refresh_tex();
                if (s_terraCanvasTex) {
                    // Canvas: riga 0=Z=0=sud → flip Y per display nord-up
                    float cu_top    = (worldSz > 0.0f) ? (s_miniPanZ + viewSzW) / worldSz : 1.0f;
                    float cu_bottom = (worldSz > 0.0f) ?  s_miniPanZ             / worldSz : 0.0f;
                    ImVec2 cuMin(ux0, cu_top), cuMax(ux1, cu_bottom);
                    dl->AddImage((ImTextureID)s_terraCanvasTex,
                        cpos, ImVec2(cpos.x + sz, cpos.y + sz),
                        cuMin, cuMax, IM_COL32(255, 255, 255, 255));
                }
            }

            // Player arrow (coordinate remappate con zoom/pan)
            bool playerValid = (playerX > -50000.0f) && (worldSz > 0.0f) && (viewSzW > 0.0f);
            if (playerValid) {
                float dpx = cpos.x + (playerX - s_miniPanX) / viewSzW * sz;
                float dpz = cpos.y + (1.0f - (playerZ - s_miniPanZ) / viewSzW) * sz;
                if (dpx >= cpos.x && dpx <= cpos.x + sz && dpz >= cpos.y && dpz <= cpos.y + sz) {
                    // Direzione camera orizzontale (forward = punta freccia)
                    float aFx = 0.0f, aFz = -1.0f; // default: nord
                    {
                        IDirect3DDevice9* pD3D = *ppD3DDevice;
                        if (pD3D) {
                            D3DMATRIX mv; pD3D->GetTransform(D3DTS_VIEW, &mv);
                            float hlen = sqrtf(mv._13*mv._13 + mv._33*mv._33);
                            if (hlen > 0.01f) { aFx = mv._13/hlen; aFz = mv._33/hlen; }
                        }
                    }
                    // Minimap: X→screen X, Z→screen Y (invertito). Fwd su screen = (aFx, -aFz)
                    float sfx =  aFx, sfy = -aFz;
                    // Perp (dx, dy) = (-sfy, sfx)
                    const float TIP = 9.0f, BASE_HALF = 5.0f, BASE_BACK = 4.0f;
                    ImVec2 tip   (dpx + sfx * TIP,        dpz + sfy * TIP);
                    ImVec2 baseL (dpx - sfx * BASE_BACK + (-sfy) * (-BASE_HALF),
                                  dpz - sfy * BASE_BACK + ( sfx) * (-BASE_HALF));
                    ImVec2 baseR (dpx - sfx * BASE_BACK + (-sfy) * ( BASE_HALF),
                                  dpz - sfy * BASE_BACK + ( sfx) * ( BASE_HALF));
                    dl->AddTriangleFilled(tip, baseL, baseR, IM_COL32(0, 255, 80, 230));
                    dl->AddTriangle(tip, baseL, baseR, IM_COL32(255, 255, 255, 200), 1.5f);
                }
            }

            if (ImGui::IsItemHovered()) {
                ImVec2 mp = ImGui::GetMousePos();

                // Zoom con scroll wheel centrato sul punto del mouse
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f && viewSzW > 0.0f) {
                    float oldViewSzW = viewSzW;
                    float factor = (wheel > 0.0f) ? 1.2f : (1.0f / 1.2f);
                    s_miniZoom *= factor;
                    s_miniZoom = ImClamp(s_miniZoom, 1.0f, 32.0f);
                    float newViewSzW = (worldSz > 0.0f) ? worldSz / s_miniZoom : worldSz;
                    float u_m = (mp.x - cpos.x) / sz;
                    float v_m = (mp.y - cpos.y) / sz;
                    float mwX = s_miniPanX + u_m * oldViewSzW;
                    float mwZ = s_miniPanZ + (1.0f - v_m) * oldViewSzW;
                    s_miniPanX = mwX - u_m * newViewSzW;
                    s_miniPanZ = mwZ - (1.0f - v_m) * newViewSzW;
                    float clampMax = worldSz - newViewSzW;
                    s_miniPanX = ImClamp(s_miniPanX, 0.0f, max(0.0f, clampMax));
                    s_miniPanZ = ImClamp(s_miniPanZ, 0.0f, max(0.0f, clampMax));
                    viewSzW = newViewSzW;
                }

                // Cerchio brush (dimensione in pixel proporzionale al raggio in world)
                float brushPx = (viewSzW > 0.0f && vs > 0.0f)
                    ? s_terraBrushR * vs / viewSzW * sz
                    : s_terraBrushR * sz / (float)N;
                dl->AddCircle(mp, brushPx, IM_COL32(255,255,80,200), 32, 1.5f);

                // Converti mouse → cella heightmap (con zoom/pan)
                float u  = (mp.x - cpos.x) / sz;
                float v2 = (mp.y - cpos.y) / sz;
                float worldX = (viewSzW > 0.0f) ? s_miniPanX + u * viewSzW : u * worldSz;
                float worldZ = (viewSzW > 0.0f) ? s_miniPanZ + (1.0f - v2) * viewSzW : (1.0f - v2) * worldSz;
                int bx = (vs > 0.0f) ? (int)(worldX / vs) : (int)(worldX / worldSz * (float)N);
                int bz = (vs > 0.0f) ? (int)(worldZ / vs) : (int)(worldZ / worldSz * (float)N);
                if (bx >= 0 && bx < N && bz >= 0 && bz < N) {
                    isPainting = ImGui::IsMouseDown(0);
                    if (isPainting) {
                        if (!s_wasPainting) terra_undo_begin();  // snapshot PRIMA della prima pennellata
                        terra_apply_brush(bx, bz);
                    }
                    ImGui::SetTooltip("HM(%d,%d)  H=%d  World(%.1f,%.1f)  Type=%d",
                        bx, bz, (int)hm[bz*N+bx],
                        worldX, worldZ,
                        tm ? (int)tm[bz*N+bx] : 0);
                }
            }

            // Pan con drag destro
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right) && viewSzW > 0.0f) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                s_miniPanX -= delta.x / sz * viewSzW;
                s_miniPanZ += delta.y / sz * viewSzW;
                float clampMax = worldSz - viewSzW;
                s_miniPanX = ImClamp(s_miniPanX, 0.0f, max(0.0f, clampMax));
                s_miniPanZ = ImClamp(s_miniPanZ, 0.0f, max(0.0f, clampMax));
            }

            if (useTipoCanvas)
                ImGui::TextDisabled("Modo Tipo: canvas typeMap. Usa 'Esporta Alpha DDS' per applicare i cambiamenti.");
            else if (!useMini)
                ImGui::TextDisabled("Vista heightmap (minimap non trovata per mappa %u)", (unsigned)mapId);
        } else {
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Nessuna texture (D3D non pronto o minimap mancante).");
        }

        // Undo stroke management
        if (!isPainting && s_wasPainting)  terra_undo_end();
        s_wasPainting = isPainting;

        ImGui::Separator();

        // ── Bottoni ──
        float bw3 = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;

        // Undo
        bool canUndo = !s_undoStack.empty();
        if (!canUndo) ImGui::BeginDisabled();
        if (ImGui::Button("Undo (Ctrl+Z)", ImVec2(bw3, 0))) terra_undo();
        if (!canUndo) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Annulla ultima pennellata (%d nel stack)", (int)s_undoStack.size());
        ImGui::SameLine(0.0f, 4.0f);

        // Reset all
        bool canReset = (s_terraOrigN > 0 && s_terraHasEdits);
        if (!canReset) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f,0.15f,0.10f,0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f,0.20f,0.15f,1.00f));
        if (ImGui::Button("Annulla Tutto", ImVec2(bw3, 0))) terra_reset_all();
        ImGui::PopStyleColor(2);
        if (!canReset) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ripristina terrain allo stato iniziale (quando hai aperto Terra).");
        ImGui::SameLine(0.0f, 4.0f);

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f,0.45f,0.70f,0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f,0.58f,0.85f,1.00f));
        if (ImGui::Button("Rebuild Live", ImVec2(bw3, 0)))
            terra_rebuild_live();
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Applica modifiche heightmap in-game live (altezza).");

        // Seconda riga: Esporta Alpha DDS
        float bw2 = ImGui::GetContentRegionAvail().x;

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f,0.30f,0.60f,0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f,0.40f,0.78f,1.00f));
        if (ImGui::Button("Esporta Alpha DDS##adds", ImVec2(bw2, 0))) {
            uint16_t mid = g_pPlayerData ? g_pPlayerData->mapId : (uint16_t)0;
            if (mid != 0 && mid != 0xFFFF) {
                char dllDirAlpha[MAX_PATH] = {};
                get_dll_dir(dllDirAlpha, sizeof(dllDirAlpha));
                // Crea cartella output ricorsivamente
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s\\Output_Maps", dllDirAlpha);
                CreateDirectoryA(tmp, nullptr);
                snprintf(tmp, sizeof(tmp), "%s\\Output_Maps\\World", dllDirAlpha);
                CreateDirectoryA(tmp, nullptr);
                snprintf(tmp, sizeof(tmp), "%s\\Output_Maps\\World\\Field", dllDirAlpha);
                CreateDirectoryA(tmp, nullptr);
                char outDir[512];
                snprintf(outDir, sizeof(outDir), "%s\\Output_Maps\\World\\Field\\%u",
                    dllDirAlpha, (unsigned)mid);
                CreateDirectoryA(outDir, nullptr);
                // srcDir = directory originale del gioco (Data\World\Field\{mapId})
                char srcDir[512];
                snprintf(srcDir, sizeof(srcDir), "%s\\Data\\World\\Field\\%u",
                    dllDirAlpha, (unsigned)mid);
                terra_export_alpha_dds(outDir, srcDir, mid);
            } else {
                snprintf(s_terraMsg, sizeof(s_terraMsg), "mapId non valido (%u)", (unsigned)mid);
            }
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Genera {mapId}_{row}{col}_A{T}.dds per ogni patch e ogni tipo.\n"
                "Output: DLL_DIR\\Output_Maps\\World\\Field\\{mapId}\\\n"
                "1) Copia la cartella World\\ in data\\ del gioco\n"
                "2) Inseriscili nel SAH con il tool (path: Field/{mapId}/{mapId}_{rc}_A{T}.dds)\n"
                "3) Ricarica la mappa → i tipi dipinti appaiono in-game.");

        if (s_terraMsg[0]) {
            ImGui::TextWrapped("%s", s_terraMsg);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f,0.50f,0.50f,1.0f));
            ImGui::TextWrapped("Per salvare usa 'Esporta WLD' nella tab Save — salva terrain + entities insieme.");
            ImGui::PopStyleColor();
        }
    }
    // ── Fine Terra helpers ────────────────────────────────────────────

    static void preview_restore()
    {
        if (!s_hijackEntity) { s_previewActive = false; return; }
        auto* b = (uint8_t*)s_hijackEntity;
        *(void**)(b + 0xA0) = s_hijackOrigModel;
        *(float*)(b+0x04) = s_hijackOrigPos[0];
        *(float*)(b+0x08) = s_hijackOrigPos[1];
        *(float*)(b+0x0c) = s_hijackOrigPos[2];
        *(float*)(b+0x10) = s_hijackOrigRot[0];
        *(float*)(b+0x14) = s_hijackOrigRot[1];
        *(float*)(b+0x18) = s_hijackOrigRot[2];
        *(float*)(b+0x1c) = s_hijackOrigUp[0];
        *(float*)(b+0x20) = s_hijackOrigUp[1];
        *(float*)(b+0x24) = s_hijackOrigUp[2];
        s_UpdateMatrix(s_hijackEntity);
        s_hijackEntity  = nullptr;
        s_previewActive = false;
        snprintf(s_previewDbg, sizeof(s_previewDbg), "preview annullata");
    }

    static void confirm_placement(const ModelEntry& m)
    {
        if (!s_hijackEntity) return;
        auto* b = (uint8_t*)s_hijackEntity;
        StagedHijack h{};
        h.entityPtr  = s_hijackEntity;
        h.origHandle = s_hijackOrigModel;
        memcpy(h.origPos, s_hijackOrigPos, 12);
        memcpy(h.origRot, s_hijackOrigRot, 12);
        memcpy(h.origUp,  s_hijackOrigUp,  12);
        h.newHandle  = m.handle;
        memcpy(h.pos, s_previewPos, 12);
        h.rot[0]  = *(float*)(b+0x10); h.rot[1]  = *(float*)(b+0x14); h.rot[2]  = *(float*)(b+0x18);
        h.rotUp[0]= *(float*)(b+0x1c); h.rotUp[1]= *(float*)(b+0x20); h.rotUp[2]= *(float*)(b+0x24);
        h.type       = m.type;
        h.modelIndex = m.index;
        g_placements.push_back(h);
        s_hijackEntity  = nullptr;
        s_previewActive = false;
        snprintf(s_previewDbg, sizeof(s_previewDbg),
            "Confermato [%s] @ (%.1f,%.1f,%.1f) — totale: %d",
            type_str(m.type), h.pos[0], h.pos[1], h.pos[2], (int)g_placements.size());
    }

    // Piazza immediatamente (senza richiedere conferma) e mantiene gizmo attivo sul nuovo placement.
    static void place_model_immediate(const ModelEntry& m)
    {
        if (!s_hijackEntity) return;
        auto* b = (uint8_t*)s_hijackEntity;
        StagedHijack h{};
        h.entityPtr  = s_hijackEntity;
        h.origHandle = s_hijackOrigModel;
        memcpy(h.origPos, s_hijackOrigPos, 12);
        memcpy(h.origRot, s_hijackOrigRot, 12);
        memcpy(h.origUp,  s_hijackOrigUp,  12);
        h.newHandle  = m.handle;
        memcpy(h.pos, s_previewPos, 12);
        h.rot[0]  = *(float*)(b+0x10); h.rot[1]  = *(float*)(b+0x14); h.rot[2]  = *(float*)(b+0x18);
        h.rotUp[0]= *(float*)(b+0x1c); h.rotUp[1]= *(float*)(b+0x20); h.rotUp[2]= *(float*)(b+0x24);
        // Salva rotazione iniziale per "Reset Rot"
        memcpy(h.initRot, h.rot,   12);
        memcpy(h.initUp,  h.rotUp, 12);
        h.type       = m.type;
        h.modelIndex = m.index;
        g_placements.push_back(h);
        s_selectedPlacementIdx = (int)g_placements.size() - 1;
        // NON resetta s_hijackEntity / s_previewActive: il gizmo rimane attivo
        const char* fn = model_filename(m.handle);
        snprintf(s_previewDbg, sizeof(s_previewDbg),
            "[%s] piazzato #%d — %d totale",
            fn ? fn : type_str(m.type), s_selectedPlacementIdx + 1, (int)g_placements.size());
    }

    // Seleziona un placement esistente per la modifica (sposta il gizmo su di esso).
    static void select_placement(int idx)
    {
        if (idx < 0 || idx >= (int)g_placements.size()) return;
        auto& h = g_placements[idx];
        s_hijackEntity    = h.entityPtr;
        s_hijackOrigModel = h.origHandle;
        memcpy(s_hijackOrigPos, h.origPos, 12);
        memcpy(s_hijackOrigRot, h.origRot, 12);
        memcpy(s_hijackOrigUp,  h.origUp,  12);
        memcpy(s_previewPos, h.pos, 12);
        s_previewActive        = true;
        s_selectedPlacementIdx = idx;
        const char* fn = model_filename(h.newHandle);
        snprintf(s_previewDbg, sizeof(s_previewDbg),
            "Selezionato #%d [%s]", idx + 1, fn ? fn : type_str(h.type));
    }

    static void remove_placement(int idx)
    {
        if (idx < 0 || idx >= (int)g_placements.size()) return;
        auto& h = g_placements[idx];
        auto* b = (uint8_t*)h.entityPtr;
        *(void**)(b + 0xA0) = h.origHandle;
        *(float*)(b+0x04) = h.origPos[0]; *(float*)(b+0x08) = h.origPos[1]; *(float*)(b+0x0c) = h.origPos[2];
        *(float*)(b+0x10) = h.origRot[0]; *(float*)(b+0x14) = h.origRot[1]; *(float*)(b+0x18) = h.origRot[2];
        *(float*)(b+0x1c) = h.origUp[0];  *(float*)(b+0x20) = h.origUp[1];  *(float*)(b+0x24) = h.origUp[2];
        s_UpdateMatrix(h.entityPtr);
        g_placements.erase(g_placements.begin() + idx);
    }

    static bool preview_show(const ModelEntry& m,
                              const float* initFwd = nullptr,
                              const float* initUp  = nullptr)
    {
        if (!shaiya::g_pWorldMgr->user) {
            snprintf(s_previewDbg, sizeof(s_previewDbg), "FAIL: player null");
            return false;
        }
        if (!m.handle) {
            snprintf(s_previewDbg, sizeof(s_previewDbg), "FAIL: handle null");
            return false;
        }

        if (!s_hijackEntity) {
            float px = shaiya::g_pWorldMgr->user->pos.x;
            float pz = shaiya::g_pWorldMgr->user->pos.z;
            // Due passaggi: prima cerca entità dello stesso tipo (rendering compatibile),
            // poi fallback su qualsiasi tipo. Mischiare tipi (es. wall in tree-slot)
            // causa crash nel renderer type-specific del gioco.
            void* best = nullptr; float bestD = 1e9f;
            for (int pass = 0; pass < 2 && !best; pass++) {
                bestD = 1e9f;
                for (auto& e : g_ents) {
                    if (!e.ptr) continue;
                    // Skip entity se la sua posizione corrisponde a un placement salvato
                    // (sessione corrente via g_placements, o sessioni precedenti via sidecar).
                    if (saved_mods_contains(e.pos)) continue;
                    bool staged = false;
                    for (auto& h : g_placements)
                        if (h.entityPtr == e.ptr) { staged = true; break; }
                    if (staged) continue;
                    if (pass == 0 && e.type != m.type) continue;
                    float dx = e.pos[0]-px, dz = e.pos[2]-pz;
                    float d = dx*dx + dz*dz;
                    if (d < bestD) { bestD = d; best = e.ptr; }
                }
            }
            if (!best) {
                snprintf(s_previewDbg, sizeof(s_previewDbg),
                    "FAIL: nessuna entity libera (g_ents=%d staged=%d)",
                    (int)g_ents.size(), (int)g_placements.size());
                return false;
            }
            s_hijackEntity     = best;
            auto* b = (uint8_t*)best;
            s_hijackOrigModel  = *(void**)(b + 0xA0);
            s_hijackOrigPos[0] = *(float*)(b+0x04); s_hijackOrigPos[1] = *(float*)(b+0x08); s_hijackOrigPos[2] = *(float*)(b+0x0c);
            s_hijackOrigRot[0] = *(float*)(b+0x10); s_hijackOrigRot[1] = *(float*)(b+0x14); s_hijackOrigRot[2] = *(float*)(b+0x18);
            s_hijackOrigUp[0]  = *(float*)(b+0x1c); s_hijackOrigUp[1]  = *(float*)(b+0x20); s_hijackOrigUp[2]  = *(float*)(b+0x24);
        }

        auto* b = (uint8_t*)s_hijackEntity;
        *(void**)(b + 0xA0) = m.handle;
        float px = shaiya::g_pWorldMgr->user->pos.x;
        float py = shaiya::g_pWorldMgr->user->pos.y;
        float pz = shaiya::g_pWorldMgr->user->pos.z;

        // Direzione orizzontale camera (per posizione e rotazione iniziale)
        float camFx = 0.0f, camFz = 1.0f;
        {
            IDirect3DDevice9* pD3D = *ppD3DDevice;
            if (pD3D) {
                D3DMATRIX mv; pD3D->GetTransform(D3DTS_VIEW, &mv);
                float hlen = sqrtf(mv._13*mv._13 + mv._33*mv._33);
                if (hlen > 0.01f) { camFx = mv._13/hlen; camFz = mv._33/hlen; }
            }
        }
        s_previewPos[0] = px + camFx * 8.0f;
        s_previewPos[2] = pz + camFz * 8.0f;
        s_previewPos[1] = terrain_height_at(s_previewPos[0], s_previewPos[2]);

        // rotFwd: initFwd > direzione camera
        float rx, ry, rz, ux, uy, uz;
        if (initFwd) {
            rx = initFwd[0]; ry = initFwd[1]; rz = initFwd[2];
            ux = initUp ? initUp[0] : 0.0f;
            uy = initUp ? initUp[1] : 1.0f;
            uz = initUp ? initUp[2] : 0.0f;
        } else {
            rx = camFx; ry = 0.0f; rz = camFz;
            ux = 0.0f;  uy = 1.0f; uz = 0.0f;
            // Se il modello è un "wall" con asse Z molto più lungo di X (es. a3_castle_Wall01:
            // X=3.84=spessore, Z=31.95=lunghezza), il lato lungo è lungo Z locale.
            // Con rotFwd=camera il lato lungo punta VERSO il player → appare "di taglio".
            // Correggiamo con +90°Y: rotFwd diventa perpendicolare alla camera, il lato
            // lungo si posiziona laterale → il player vede la faccia del muro. ✓
            {
                const char* wfn = model_filename(m.handle);
                bool isWall = wfn && (strstr(wfn,"wall") || strstr(wfn,"Wall") || strstr(wfn,"WALL"));
                if (isWall) {
                    float aabb[6];
                    if (get_model_aabb(m.handle, aabb)) {
                        float sX = aabb[3]-aabb[0];
                        float sZ = aabb[5]-aabb[2];
                        if (sZ > sX * 2.0f) {
                            // +90°Y: (camFx,0,camFz) → (camFz,0,-camFx)
                            rx = camFz; rz = -camFx;
                        }
                    }
                }
            }
        }

        // Allinea alla pendenza del terreno se richiesto
        if (s_alignToSlope) {
            float nx, ny, nz;
            terrain_normal_at(s_previewPos[0], s_previewPos[2], nx, ny, nz);
            ux = nx; uy = ny; uz = nz;
            float dot = rx*nx + ry*ny + rz*nz;
            float prx=rx-dot*nx, pry=ry-dot*ny, prz=rz-dot*nz;
            float prl=sqrtf(prx*prx+pry*pry+prz*prz);
            if (prl > 0.001f) { rx=prx/prl; ry=pry/prl; rz=prz/prl; }
        }

        *(float*)(b+0x04) = s_previewPos[0]; *(float*)(b+0x08) = s_previewPos[1]; *(float*)(b+0x0c) = s_previewPos[2];
        *(float*)(b+0x10) = rx; *(float*)(b+0x14) = ry; *(float*)(b+0x18) = rz;
        *(float*)(b+0x1c) = ux; *(float*)(b+0x20) = uy; *(float*)(b+0x24) = uz;
        s_UpdateMatrix(s_hijackEntity);

        s_previewActive = true;
        const char* fn = model_filename(m.handle);
        snprintf(s_previewDbg, sizeof(s_previewDbg), "Preview [%s]  %s",
            type_str(m.type), fn ? fn : "(no filename)");
        return true;
    }

    // ── Stato placement + lista piazzati per la sezione corrente ──
    static void draw_preview_controls(EditorSection sec)
    {
        ImVec4 sc = s_previewActive
            ? ImVec4(0.2f,1.0f,0.5f,1.0f)
            : (strncmp(s_previewDbg,"FAIL",4)==0 ? ImVec4(1,0.3f,0.3f,1) : ImVec4(0.5f,0.5f,0.5f,1));
        ImGui::TextColored(sc, "%s", s_previewDbg);
        ImGui::SameLine();
        ImGui::Checkbox("Allinea pendenza##mod", &s_alignToSlope);

        // Lista piazzati per questa sezione (cliccabili per selezionare/rieditare)
        MapEntityType mtype = section_to_type(sec);
        bool hasPiazzati = false;
        for (auto& h : g_placements) if (h.type == mtype) { hasPiazzati = true; break; }
        if (hasPiazzati) {
            ImGui::TextDisabled("Piazzati:");
            for (int i = 0; i < (int)g_placements.size(); i++) {
                auto& h = g_placements[i];
                if (h.type != mtype) continue;
                bool isSel = (i == s_selectedPlacementIdx);
                const char* fn = model_filename(h.newHandle);
                const char* base = fn ? strrchr(fn, '/') : nullptr;
                const char* name = base ? base+1 : (fn ? fn : "?");
                char lbl[80]; snprintf(lbl, 80, "#%d %s##pi%d", i+1, name, i);
                if (ImGui::Selectable(lbl, isSel, ImGuiSelectableFlags_None, ImVec2(0,0))) {
                    if (!isSel) select_placement(i);
                    else { s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1; }
                }
            }
        }
        ImGui::Separator();
    }

    // ── Scatter Erba: undo ────────────────────────────────────────────
    static void scatter_undo()
    {
        if (s_scUndoBatches.empty()) {
            snprintf(s_scMsg, sizeof(s_scMsg), "Niente da annullare.");
            return;
        }
        int count = s_scUndoBatches.back();
        s_scUndoBatches.pop_back();
        int start = (int)g_placements.size() - count;
        if (start < 0) start = 0;
        for (int i = start; i < (int)g_placements.size(); i++) {
            auto& h = g_placements[i];
            auto* b = (uint8_t*)h.entityPtr;
            *(void**)(b + 0xA0) = h.origHandle;
            *(float*)(b + 0x04) = h.origPos[0];
            *(float*)(b + 0x08) = h.origPos[1];
            *(float*)(b + 0x0c) = h.origPos[2];
            *(float*)(b + 0x10) = h.origRot[0];
            *(float*)(b + 0x14) = h.origRot[1];
            *(float*)(b + 0x18) = h.origRot[2];
            *(float*)(b + 0x1c) = h.origUp[0];
            *(float*)(b + 0x20) = h.origUp[1];
            *(float*)(b + 0x24) = h.origUp[2];
            s_UpdateMatrix(h.entityPtr);
        }
        g_placements.resize((size_t)(start < 0 ? 0 : start));
        snprintf(s_scMsg, sizeof(s_scMsg), "Annullati %d modelli (rimasti: %d).",
            count, (int)g_placements.size());
    }

    // ── Scatter Erba: pianta un brush ────────────────────────────────
    static void scatter_apply_brush(float worldX, float worldZ)
    {
        int N = terra_N();
        float vs = *(float*)0x022F227C;
        if (N <= 0 || vs < 0.1f) return;
        uint8_t* tm = terra_tm();
        int16_t* hm = terra_hm();
        if (!tm || !hm) { snprintf(s_scMsg, sizeof(s_scMsg), "Heightmap/typemap non disponibili."); return; }

        // Trova handle modello selezionato
        void*         scHandle = nullptr;
        MapEntityType scType   = MapEntityType::Grass;
        {
            int gi = 0;
            for (auto& m : g_models) {
                if (m.type != MapEntityType::Grass && m.type != MapEntityType::Vani) continue;
                if (gi == s_scModelSel) { scHandle = m.handle; scType = m.type; break; }
                gi++;
            }
        }
        if (!scHandle) { snprintf(s_scMsg, sizeof(s_scMsg), "Seleziona un modello Grass prima."); return; }


        int   R     = (int)ceilf(s_scBrushR);
        int   cx0   = (int)(worldX / vs + 0.5f);
        int   cz0   = (int)(worldZ / vs + 0.5f);
        float minSq = s_scMinSpacing * s_scMinSpacing;

        // Posizioni Grass già piazzate (per spacing check)
        std::vector<float> existX, existZ;
        for (auto& h : g_placements) {
            if (h.type == MapEntityType::Grass || h.type == MapEntityType::Vani) {
                existX.push_back(h.pos[0]); existZ.push_back(h.pos[2]);
            }
        }

        int added = 0;
        for (int dz = -R; dz <= R; dz++) {
            for (int dx = -R; dx <= R; dx++) {
                if (sqrtf((float)(dx*dx + dz*dz)) > s_scBrushR) continue;
                int cx = cx0 + dx, cz = cz0 + dz;
                if (cx < 0 || cx >= N || cz < 0 || cz >= N) continue;

                uint8_t ttype = tm[(size_t)cz * N + cx];
                if (ttype >= 16 || !s_scTypeFilter[ttype]) continue;
                if ((float)rand() / RAND_MAX >= s_scDensity) continue;

                // Posizione world con jitter sub-cella
                float wx = ((float)cx + (float)rand()/RAND_MAX - 0.5f) * vs;
                float wz = ((float)cz + (float)rand()/RAND_MAX - 0.5f) * vs;
                float wy = terrain_height_at(wx, wz);

                // Spacing check
                bool tooClose = false;
                for (int si = 0; si < (int)existX.size() && !tooClose; si++) {
                    float ddx = wx - existX[si], ddz = wz - existZ[si];
                    if (ddx*ddx + ddz*ddz < minSq) tooClose = true;
                }
                if (tooClose) continue;

                // Trova entity Grass libera PIÙ VICINA al target XZ.
                // Entity vicina = stesso settore visivo → appare in live + Y del terreno corretto.
                void*          freeEnt = nullptr;
                CapturedEntity freeCapt{};
                float          freeEntDist2 = 1e9f;
                for (auto& e : g_ents) {
                    if (!e.ptr) continue;
                    if (e.type != MapEntityType::Grass && e.type != MapEntityType::Vani) continue;
                    bool inUse = false;
                    for (auto& h2 : g_placements) if (h2.entityPtr == e.ptr) { inUse = true; break; }
                    if (inUse) continue;
                    if (saved_mods_contains(e.pos)) continue;
                    float dx = e.pos[0] - wx, dz = e.pos[2] - wz;
                    float d2 = dx*dx + dz*dz;
                    if (d2 < freeEntDist2) { freeEntDist2 = d2; freeEnt = e.ptr; freeCapt = e; }
                }
                if (!freeEnt) {
                    snprintf(s_scMsg, sizeof(s_scMsg),
                        "Entity Grass esaurite (%d piazzati). Esporta WLD per liberare slot.", added);
                    return;
                }

                // Rotazione Y random
                float angle = s_scRotY ? ((float)rand() / RAND_MAX * 6.28318f) : 0.0f;

                StagedHijack h{};
                h.entityPtr  = freeEnt;
                h.origHandle = freeCapt.modelHandle;
                memcpy(h.origPos, freeCapt.pos,   12);
                memcpy(h.origRot, freeCapt.rot,   12);
                memcpy(h.origUp,  freeCapt.rotUp, 12);
                h.newHandle  = scHandle;
                h.pos[0] = wx; h.pos[1] = wy;  h.pos[2] = wz;
                float fwdX = sinf(angle), fwdZ = cosf(angle);
                if (s_scAlignSlope) {
                    float nx, ny, nz;
                    terrain_normal_at(wx, wz, nx, ny, nz);
                    h.rotUp[0]=nx; h.rotUp[1]=ny; h.rotUp[2]=nz;
                    float dot=fwdX*nx+0.0f*ny+fwdZ*nz;
                    float prx=fwdX-dot*nx, pry=-dot*ny, prz=fwdZ-dot*nz;
                    float prl=sqrtf(prx*prx+pry*pry+prz*prz);
                    if (prl>0.001f){h.rot[0]=prx/prl;h.rot[1]=pry/prl;h.rot[2]=prz/prl;}
                    else            {h.rot[0]=fwdX;h.rot[1]=0.0f;h.rot[2]=fwdZ;}
                } else {
                    h.rot[0]=fwdX; h.rot[1]=0.0f; h.rot[2]=fwdZ;
                    h.rotUp[0]=0.0f; h.rotUp[1]=1.0f; h.rotUp[2]=0.0f;
                }
                memcpy(h.initRot, h.rot,   12);
                memcpy(h.initUp,  h.rotUp, 12);
                h.type       = scType;
                h.modelIndex = s_scModelSel;

                auto* b = (uint8_t*)freeEnt;
                *(void**)(b + 0xA0) = scHandle;
                *(float*)(b + 0x04) = wx;
                *(float*)(b + 0x08) = wy;
                *(float*)(b + 0x0c) = wz;
                *(float*)(b + 0x10) = h.rot[0];
                *(float*)(b + 0x14) = h.rot[1];
                *(float*)(b + 0x18) = h.rot[2];
                *(float*)(b + 0x1c) = h.rotUp[0];
                *(float*)(b + 0x20) = h.rotUp[1];
                *(float*)(b + 0x24) = h.rotUp[2];
                s_UpdateMatrix(freeEnt);

                g_placements.push_back(h);
                existX.push_back(wx); existZ.push_back(wz);
                added++;
            }
        }
        if (added > 0)
            snprintf(s_scMsg, sizeof(s_scMsg),
                "+%d piazzati (tot: %d). Ctrl+Z annulla.", added, (int)g_placements.size());
    }

    // ── Scatter Erba: pannello principale ────────────────────────────
    static void draw_content_scatter()
    {
        uint16_t mapId = get_current_map_id();
        if (mapId != 0xFFFF) terra_load_type_names(mapId);

        // Ctrl+Z undo
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            scatter_undo();

        ImGui::TextColored(ImVec4(0.5f,1.0f,0.5f,1.0f), "Scatter Erba");
        ImGui::TextDisabled("Dipingi modelli Grass solo sui terreni abilitati.");
        ImGui::Separator();

        // Parametri pennello
        ImGui::PushItemWidth(200.0f);
        ImGui::SliderFloat("Raggio (celle)##sc", &s_scBrushR,     1.0f, 50.0f, "%.0f");
        ImGui::SliderFloat("Densita'##sc",        &s_scDensity,   0.01f,  1.0f, "%.2f");
        ImGui::SliderFloat("Spaz. min (m)##sc",   &s_scMinSpacing, 0.5f, 30.0f, "%.1f");
        ImGui::PopItemWidth();
        ImGui::Checkbox("Rot. Y random##sc", &s_scRotY);
        ImGui::SameLine();
        ImGui::Checkbox("Allinea pendenza##sc", &s_scAlignSlope);

        ImGui::Separator();

        // Filtro tipi terreno
        ImGui::TextDisabled("Pianta su (terreni abilitati):");
        if (s_terraTypeCount == 0) {
            ImGui::TextColored(ImVec4(1.0f,0.6f,0.0f,1.0f),
                "Nessun tipo rilevato. Apri Terra > Tipo prima.");
        } else {
            // Auto-init: abilita tipi erbosi al primo accesso
            if (!s_scFilterInited) {
                s_scFilterInited = true;
                for (int i = 0; i < s_terraTypeCount && i < 16; i++) {
                    const char* nm = s_terraTypeNames[i];
                    s_scTypeFilter[i] =
                        (strstr(nm,"grass") || strstr(nm,"Grass") ||
                         strstr(nm,"flower")|| strstr(nm,"Flower")||
                         strstr(nm,"jungle")|| strstr(nm,"earth") ||
                         strstr(nm,"base")  || strstr(nm,"glass") ||
                         strstr(nm,"plant") || strstr(nm,"prato"));
                }
            }
            ImGui::Columns(2, "scfc", false);
            for (int i = 0; i < s_terraTypeCount && i < 16; i++) {
                ImVec4 tc = ImGui::ColorConvertU32ToFloat4(kTerraTypeColors[i % kTerraNumTypes]);
                ImGui::PushStyleColor(ImGuiCol_CheckMark, tc);
                char lbl[300];
                snprintf(lbl, sizeof(lbl), "[%d] %s##scf", i, s_terraTypeNames[i]);
                ImGui::Checkbox(lbl, &s_scTypeFilter[i]);
                ImGui::PopStyleColor();
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        }

        ImGui::Separator();

        // Selezione modello Grass
        ImGui::TextDisabled("Modello Grass da piazzare:");
        ImGui::BeginChild("##scModList", ImVec2(0.0f, 110.0f), true);
        {
            int gi = 0;
            for (auto& m : g_models) {
                if (m.type != MapEntityType::Grass && m.type != MapEntityType::Vani) continue;
                const char* fn = model_filename(m.handle);
                const char* disp = fn
                    ? (strrchr(fn,'\\') ? strrchr(fn,'\\')+1 : fn)
                    : "(handle)";
                bool sel = (gi == s_scModelSel);
                if (ImGui::Selectable(disp, sel))
                    s_scModelSel = sel ? -1 : gi;
                gi++;
            }
            if (gi == 0)
                ImGui::TextDisabled("Nessun modello. Apri sezione Grass prima.");
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Undo button
        bool hasUndo = !s_scUndoBatches.empty();
        if (!hasUndo) ImGui::BeginDisabled();
        if (ImGui::Button("Annulla ultimo##sc")) scatter_undo();
        if (!hasUndo) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(Ctrl+Z)  Batch: %d", (int)s_scUndoBatches.size());

        ImGui::Separator();

        // Minimappa (canvas tipo, uguale a Terra > Tipo)
        int  Ni     = terra_N();
        float vs2   = *(float*)0x022F227C;
        float wldSz = (Ni > 0 && vs2 > 0.1f) ? (float)Ni * vs2 : 0.0f;

        float plX = -99999.0f, plZ = -99999.0f;
        {
            void* pU = *(void**)0x904AA0;
            if (pU && (uintptr_t)pU > 0xFFFFu) {
                plX = *(float*)((uint8_t*)pU + 0x10);
                plZ = *(float*)((uint8_t*)pU + 0x18);
            }
        }

        float avail2 = ImGui::GetContentRegionAvail().x - 4.0f;
        float sz2    = avail2 < 512.0f ? avail2 : 512.0f;

        ImGui::Text("Zoom:%.1fx", s_scMiniZoom); ImGui::SameLine();
        if (ImGui::SmallButton("1:1##sczr"))
            { s_scMiniZoom=1.0f; s_scMiniPanX=0.0f; s_scMiniPanZ=0.0f; }
        ImGui::SameLine();
        ImGui::TextDisabled("Scroll=zoom  R-drag=sposta  L-clic=pianta");

        // Canvas tipo (refresh se necessario)
        if (s_terraCanvasDirty || !s_terraCanvasTex) terra_refresh_tex(true);
        IDirect3DTexture9* dispTex = s_terraCanvasTex;

        if (!dispTex) {
            ImGui::TextColored(ImVec4(1.0f,0.5f,0.0f,1.0f),
                "Canvas non disponibile. Apri Terra > Tipo prima.");
        } else if (sz2 > 0.0f && wldSz > 0.0f) {
            float viewW = wldSz / s_scMiniZoom;
            float ux0   = s_scMiniPanX / wldSz;
            float ux1   = ux0 + viewW / wldSz;
            // Canvas: riga 0=Z=0=sud → flip Y per nord-up
            float uyT   = (s_scMiniPanZ + viewW) / wldSz;
            float uyB   =  s_scMiniPanZ           / wldSz;

            ImVec2 cpos2 = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)dispTex, ImVec2(sz2,sz2),
                ImVec2(ux0,uyT), ImVec2(ux1,uyB));
            ImDrawList* dl2 = ImGui::GetWindowDrawList();

            // Dots: placement scatter già eseguiti
            for (auto& h : g_placements) {
                if (h.type != MapEntityType::Grass && h.type != MapEntityType::Vani) continue;
                float px2 = cpos2.x + (h.pos[0] - s_scMiniPanX) / viewW * sz2;
                float pz2 = cpos2.y + (1.0f - (h.pos[2] - s_scMiniPanZ) / viewW) * sz2;
                if (px2 >= cpos2.x-2 && px2 <= cpos2.x+sz2+2 &&
                    pz2 >= cpos2.y-2 && pz2 <= cpos2.y+sz2+2)
                    dl2->AddCircleFilled({px2,pz2}, 2.0f, IM_COL32(0,220,100,200));
            }

            // Player arrow
            if (plX > -50000.0f) {
                float dpx2 = cpos2.x + (plX - s_scMiniPanX) / viewW * sz2;
                float dpz2 = cpos2.y + (1.0f - (plZ - s_scMiniPanZ) / viewW) * sz2;
                if (dpx2 >= cpos2.x && dpx2 <= cpos2.x+sz2 &&
                    dpz2 >= cpos2.y && dpz2 <= cpos2.y+sz2) {
                    float aFx=0.0f, aFz=-1.0f;
                    IDirect3DDevice9* pD3D = *ppD3DDevice;
                    if (pD3D) {
                        D3DMATRIX mv; pD3D->GetTransform(D3DTS_VIEW, &mv);
                        float hl = sqrtf(mv._13*mv._13+mv._33*mv._33);
                        if (hl > 0.01f) { aFx=mv._13/hl; aFz=mv._33/hl; }
                    }
                    float sfx=aFx, sfy=-aFz;
                    const float TIP=9.0f, BH=5.0f, BB=4.0f;
                    ImVec2 tip2(dpx2+sfx*TIP, dpz2+sfy*TIP);
                    ImVec2 bL2(dpx2-sfx*BB+(-sfy)*(-BH), dpz2-sfy*BB+sfx*(-BH));
                    ImVec2 bR2(dpx2-sfx*BB+(-sfy)*( BH), dpz2-sfy*BB+sfx*( BH));
                    dl2->AddTriangleFilled(tip2,bL2,bR2, IM_COL32(0,255,80,230));
                    dl2->AddTriangle(tip2,bL2,bR2, IM_COL32(255,255,255,200), 1.5f);
                }
            }

            // Interazione mouse
            if (ImGui::IsItemHovered()) {
                ImVec2 mp2 = ImGui::GetMousePos();
                float  u2  = (mp2.x - cpos2.x) / sz2;
                float  v2  = (mp2.y - cpos2.y) / sz2;

                // Scroll zoom
                float wheel2 = ImGui::GetIO().MouseWheel;
                if (wheel2 != 0.0f) {
                    float oldV2 = viewW;
                    s_scMiniZoom = ImClamp(s_scMiniZoom*(wheel2>0?1.2f:1.0f/1.2f), 1.0f, 32.0f);
                    float newV2  = wldSz / s_scMiniZoom;
                    float mwX2 = s_scMiniPanX + u2*oldV2;
                    float mwZ2 = s_scMiniPanZ + (1.0f-v2)*oldV2;
                    s_scMiniPanX = ImClamp(mwX2 - u2*newV2,      0.0f, wldSz-newV2);
                    s_scMiniPanZ = ImClamp(mwZ2 - (1.0f-v2)*newV2, 0.0f, wldSz-newV2);
                }

                // Right-drag pan
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                    ImVec2 dd2 = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
                    float cv = wldSz / s_scMiniZoom;
                    s_scMiniPanX = ImClamp(s_scMiniPanX - dd2.x/sz2*cv, 0.0f, wldSz-cv);
                    s_scMiniPanZ = ImClamp(s_scMiniPanZ + dd2.y/sz2*cv, 0.0f, wldSz-cv);
                }

                // Brush circle indicator
                float brushPx = s_scBrushR * vs2 / viewW * sz2;
                dl2->AddCircle(mp2, brushPx, IM_COL32(100,255,100,180), 32, 1.5f);
                dl2->AddCircle(mp2, 3.0f, IM_COL32(255,255,255,220), 8, 2.0f);

                // Left click/drag → scatter
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (!s_scDragging) {
                        s_scDragging   = true;
                        s_scBatchStart = (int)g_placements.size();
                    }
                    float wx3 = s_scMiniPanX + u2*viewW;
                    float wz3 = s_scMiniPanZ + (1.0f-v2)*viewW;
                    scatter_apply_brush(wx3, wz3);
                }
            }

            // Mouse rilasciato → chiudi batch
            if (s_scDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                s_scDragging = false;
                int bsz = (int)g_placements.size() - s_scBatchStart;
                if (bsz > 0) s_scUndoBatches.push_back(bsz);
            }
        }

        // Messaggio stato
        if (s_scMsg[0]) { ImGui::Spacing(); ImGui::TextWrapped("%s", s_scMsg); }
    }

    // ── Content: model thumbnail grid ──
    static void draw_content_models(EditorSection sec)
    {
        // Carica sidecar mods per la mappa corrente (lazy, una sola volta per mappa)
        uint16_t curMapId = get_current_map_id();
        if (curMapId != 0xFFFF) saved_mods_load(curMapId);

        if (!g_modelsBuilt) { build_model_list(); match_fs_handles(); }
        if (!s_fsScanned)   { rebuild_fs_models(); match_fs_handles(); }

        MapEntityType mtype = section_to_type(sec);

        int total = 0, loaded = 0;
        for (auto& fm : s_fsModels) {
            if (fm.type != mtype) continue;
            total++;
            if (fm.handle) loaded++;
        }
        ImGui::Text("%d modelli | %d/%d caricati | %d piaz.", total, loaded, kPoolMax, (int)g_placements.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Ricarica##rc")) {
            for (auto& fm : s_fsModels) { fm.thumbTried = false; fm.handle = nullptr; }
            g_modelsBuilt = false; s_fsScanned = false; s_contentSel = -1;
            thumb_pool_evict_all();
            // Non ripristinare entity già piazzate — deseleziona solo
            s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1;
        }

        draw_preview_controls(sec);

        // ── Drop zone: trascina un modello qui per piazzarlo ──
        ImGui::InvisibleButton("##dropzone", ImVec2(-1.0f, 24.0f));
        ImVec2 dzMin = ImGui::GetItemRectMin();
        ImVec2 dzMax = ImGui::GetItemRectMax();
        bool dzHov = ImGui::IsItemHovered();
        {
            ImDrawList* dzdl = ImGui::GetWindowDrawList();
            ImU32 bg = dzHov ? IM_COL32(45,105,50,210) : IM_COL32(22,50,25,150);
            ImU32 bd = dzHov ? IM_COL32(90,215,100,255) : IM_COL32(45,95,50,180);
            dzdl->AddRectFilled(dzMin, dzMax, bg, 3.0f);
            dzdl->AddRect(dzMin, dzMax, bd, 3.0f);
            dzdl->AddText(ImVec2(dzMin.x + 8.0f, dzMin.y + 5.0f),
                IM_COL32(130,215,140,230),
                "  Trascina un modello qui per piazzarlo al player  ");
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MAP_MODEL_IDX")) {
                int idx = *(int*)payload->Data;
                if (idx >= 0 && idx < (int)s_fsModels.size()) {
                    s_contentSel = idx;
                    auto& fm2 = s_fsModels[idx];
                    if (!fm2.handle) {
                        void* h = dyn_load_model(fm2.type, fm2.filename);
                        if (h) fm2.handle = h;
                    }
                    if (fm2.handle) {
                        if (s_previewActive) {
                            if (s_selectedPlacementIdx >= 0) {
                                // era in edit di un placement già confermato — deseleziona senza restore
                                s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1;
                            } else {
                                preview_restore();
                            }
                        }
                        ModelEntry tmpM{ fm2.handle, fm2.type, idx };
                        if (preview_show(tmpM))
                            place_model_immediate(tmpM);
                    } else {
                        snprintf(s_previewDbg, sizeof(s_previewDbg),
                            "FAIL: caricamento '%s' fallito", fm2.filename);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        static char s_search[64] = {};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##srch", "Cerca modello...", s_search, sizeof(s_search));
        ImGui::Separator();

        std::string lwr = s_search;
        for (auto& c : lwr) c = (char)tolower((unsigned char)c);

        // Grid che riempie sempre l'intera larghezza del pannello
        const float kGap  = 4.0f;
        float availW = max(1.0f, ImGui::GetContentRegionAvail().x - 2.0f);
        int   cols   = max(1, (int)(availW / 130.0f));
        float cardW  = (availW - kGap * (float)(cols - 1)) / (float)cols;
        float thumbSz = cardW - 4.0f;
        float cardH   = thumbSz + 20.0f;

        ImGui::BeginChild("##mdlgrid", ImVec2(0, 0), false);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Scroll detection dentro il child: pausa durante scroll, 2 load/frame dopo 15f fermi.
        // Con LRU eviction D3D-safe il pool resta a max kPoolMax texture → nessun crash.
        static float   s_lastScrollY = -1.0f;
        static int     s_scrollIdleF = 0;
        static int     s_lfl = -1, s_lb = 0;
        {
            float sy = ImGui::GetScrollY();
            if (sy != s_lastScrollY) { s_lastScrollY = sy; s_scrollIdleF = 0; }
            else                     { s_scrollIdleF++; }
            int f = ImGui::GetFrameCount();
            if (f != s_lfl) {
                s_lfl = f; s_lb = (s_scrollIdleF >= 15) ? 4 : 0;
                // Evict proattivo: libera slot non visti da > 240 frame (~4 sec a 60fps)
                thumb_pool_sweep((uint32_t)f, 240u);
                // Reset thumbTried per modelli falliti ogni 300 frame (~5 sec) → riprova
                if ((f % 300) == 0)
                    for (auto& fm2 : s_fsModels)
                        if (!fm2.handle) fm2.thumbTried = false;
            }
        }

        bool isWallSec = (sec == EditorSection::Wall);

        int visIdx = 0;
        for (int i = 0; i < (int)s_fsModels.size(); i++) {
            auto& fm = s_fsModels[i];
            if (fm.type != mtype) continue;
            // Sezione Wall: mostra solo modelli con "wall" nel nome
            {
                char low[128] = {};
                for (int k = 0; fm.filename[k] && k < 127; k++)
                    low[k] = (char)tolower((unsigned char)fm.filename[k]);
                if (isWallSec && !strstr(low, "wall")) continue;
                if (!lwr.empty() && !strstr(low, lwr.c_str())) continue;
            }

            if (visIdx % cols != 0) ImGui::SameLine(0, kGap);
            ImGui::PushID(i);

            ImVec2 cp = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##c", ImVec2(cardW, cardH));
            // Auto-load solo se fermo (budget s_lb) O su click esplicito
            if (s_lb > 0 && ImGui::IsItemVisible() && !fm.handle && !fm.thumbTried) {
                fm.thumbTried = true;
                void* h = dyn_load_model(fm.type, fm.filename);
                if (h) fm.handle = h;
                s_lb--;
            }
            bool isHov  = ImGui::IsItemHovered();
            bool isClic = ImGui::IsItemClicked();
            bool sel    = (i == s_contentSel);
            bool inW    = (fm.handle != nullptr);

            // Touch LRU slot per card visibili già caricate → impedisce sweep accidentale
            if (fm.handle && ImGui::IsItemVisible()) {
                uint32_t tf = (uint32_t)ImGui::GetFrameCount();
                for (auto& ps : s_pool)
                    if (ps.mem == fm.handle) { ps.lastFrame = tf; break; }
            }

            IDirect3DTexture9* thumb = fm.handle ? get_model_thumb(fm.handle) : nullptr;
            ImVec2 te = {cp.x + cardW, cp.y + thumbSz}; // area immagine
            if (thumb) {
                dl->AddRectFilled(cp, te, IM_COL32(10,10,16,235), 4.0f);
                // ── Prospettiva angolata: muro visto leggermente di lato ──
                // Lato sinistro = "lontano" (compresso), lato destro = "vicino" (pieno)
                float imgCx = cp.x + cardW * 0.5f;
                float imgCy = cp.y + thumbSz * 0.5f;
                float hw = thumbSz * 0.41f;  // metà larghezza
                float hh = thumbSz * 0.44f;  // metà altezza

                float skew  = hw * 0.30f;    // scostamento orizzontale lato lontano
                float persp = hh * 0.18f;    // compressione verticale lato lontano

                // Trapezio prospettico (q_t* = top, q_b* = bottom, *l = sinistra, *r = destra)
                ImVec2 q_tl = { imgCx - hw + skew, imgCy - hh + persp };
                ImVec2 q_tr = { imgCx + hw,         imgCy - hh         };
                ImVec2 q_br = { imgCx + hw,         imgCy + hh         };
                ImVec2 q_bl = { imgCx - hw + skew, imgCy + hh - persp };

                dl->PushClipRect(cp, te, true);

                // Faccia principale: texture in prospettiva
                dl->AddImageQuad((ImTextureID)(UINT_PTR)thumb, q_tl, q_tr, q_br, q_bl);

                // Spigolo destra: striscia di profondità (mostra spessore)
                float ds = thumbSz * 0.036f;
                ImVec2 dep[4] = {
                    q_tr,
                    { q_tr.x + ds, q_tr.y + persp },
                    { q_br.x + ds, q_br.y - persp },
                    q_br
                };
                dl->AddConvexPolyFilled(dep, 4, IM_COL32(50,52,62,255));

                // Highlight spigolo superiore
                dl->AddLine(q_tl, q_tr, IM_COL32(255,255,255,45), 1.0f);
                // Ombra spigolo sinistro (lato lontano)
                dl->AddLine(q_tl, q_bl, IM_COL32(0,0,0,90), 0.8f);
                // Contorno spigolo destra
                dl->AddLine(q_tr, q_br, IM_COL32(0,0,0,80), 0.8f);

                dl->PopClipRect();
            } else {
                // Card non caricata: sfondo scuro + nome file centrato
                ImU32 bg = isHov ? IM_COL32(30,34,52,220) : IM_COL32(18,20,32,200);
                dl->AddRectFilled(cp, te, bg, 4.0f);
                // Nome troncato al centro dell'area thumbnail
                const char* dot2 = strrchr(fm.filename, '.');
                int blen2 = dot2 ? (int)(dot2 - fm.filename) : (int)strlen(fm.filename);
                char cn[16]; int take = blen2 < 13 ? blen2 : 12;
                strncpy_s(cn, fm.filename, take); cn[take] = 0;
                if (blen2 > 12) strcat_s(cn, "~");
                ImVec2 ts = ImGui::CalcTextSize(cn);
                dl->AddText({cp.x + (cardW - ts.x)*0.5f, cp.y + (thumbSz - ts.y)*0.5f},
                    isHov ? IM_COL32(180,200,255,230) : IM_COL32(110,120,150,200), cn);
            }

            ImU32 bdr = sel   ? IM_COL32(255,200,50,255)  :
                        isHov ? IM_COL32(170,170,255,200) :
                                IM_COL32(50,50,70,180);
            dl->AddRect(cp, te, bdr, 4.0f, 0, sel ? 2.5f : 1.0f);

            const char* dot = strrchr(fm.filename, '.');
            int blen = dot ? (int)(dot - fm.filename) : (int)strlen(fm.filename);
            char sn[12];
            if (blen > 9) {
                strncpy_s(sn, fm.filename, 8); sn[8] = 0; strcat_s(sn, "~");
            } else {
                strncpy_s(sn, fm.filename, blen); sn[blen] = 0;
            }
            // Nome sempre leggibile; thumbnail caricata = testo luminoso
            ImU32 tc = inW ? IM_COL32(210,220,240,255) : IM_COL32(130,140,160,220);
            dl->AddText({cp.x + 3.0f, cp.y + thumbSz + 3.0f}, tc, sn);

            if (isHov)
                ImGui::SetTooltip("%s\n%s\nTrascina sulla zona verde per piazzare", fm.filename, fm.relDir);

            // Click singolo: seleziona la card (nessuna preview in world)
            if (isClic) {
                s_contentSel = i;
                // Forza caricamento immediato se non ancora caricato
                if (!fm.handle && !fm.thumbTried) {
                    fm.thumbTried = true;
                    void* h = dyn_load_model(fm.type, fm.filename);
                    if (h) fm.handle = h;
                }
            }

            // Drag source: trascina la card sulla drop zone per piazzare
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("MAP_MODEL_IDX", &i, sizeof(int));
                IDirect3DTexture9* dth = fm.handle ? get_model_thumb(fm.handle) : nullptr;
                if (dth) ImGui::Image((ImTextureID)(UINT_PTR)dth, ImVec2(28.0f, 28.0f));
                else     ImGui::TextDisabled("[%s]", type_str(fm.type));
                ImGui::SameLine();
                ImGui::Text("%s", fm.filename);
                ImGui::EndDragDropSource();
            }

            ImGui::PopID();
            visIdx++;
        }
        ImGui::EndChild();
    }

    // ── Existing entities tab — lista g_ents con selezione e gizmo ──
    static void draw_existing_entities()
    {
        // Helper: nome breve dal path del modello
        auto short_name = [](void* h) -> const char* {
            const char* fn = model_filename(h);
            if (!fn) return "(sconosciuto)";
            const char* s = strrchr(fn, '/');
            return s ? s+1 : fn;
        };

        ImGuiIO& io = ImGui::GetIO();

        // ── Filtri ──
        static char  s_entFilter[64]  = {};
        static bool  s_entNearOnly    = false;
        static float s_entNearRadius  = 100.0f;
        static int   s_entTypeFilter  = -1; // -1=tutti

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##ef", s_entFilter, sizeof(s_entFilter));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filtra per nome modello");
        ImGui::Spacing();

        const char* typeLabels[] = { "Tutti","Build","Shape","Tree","Grass","Object","Vani" };
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##etf", &s_entTypeFilter, typeLabels, IM_ARRAYSIZE(typeLabels));

        ImGui::Checkbox("Solo vicini", &s_entNearOnly);
        if (s_entNearOnly) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##nr", &s_entNearRadius, 1.0f, 5.0f, 2000.0f, "%.0f u");
        }
        ImGui::Separator();

        // Posizione player per filtro "Solo vicini"
        float ppx = 0.0f, ppz = 0.0f;
        if (shaiya::g_pWorldMgr && shaiya::g_pWorldMgr->user) {
            ppx = shaiya::g_pWorldMgr->user->pos.x;
            ppz = shaiya::g_pWorldMgr->user->pos.z;
        }

        // Conteggio entità visibili
        int visCount = 0;
        for (auto& e : g_ents) {
            if (s_entNearOnly) {
                float dx=e.pos[0]-ppx, dz=e.pos[2]-ppz;
                if (sqrtf(dx*dx+dz*dz) > s_entNearRadius) continue;
            }
            if (s_entFilter[0]) {
                const char* nm = short_name(e.modelHandle);
                bool found = false;
                for (const char* p=nm; *p; p++) {
                    if (_strnicmp(p, s_entFilter, strlen(s_entFilter))==0) { found=true; break; }
                }
                if (!found) continue;
            }
            if (s_entTypeFilter > 0) {
                MapEntityType wanted = (MapEntityType)(s_entTypeFilter-1);
                if (e.type != wanted) continue;
            }
            visCount++;
        }
        ImGui::TextDisabled("%d / %d entita'", visCount, (int)g_ents.size());

        // Se selezione attiva, mostra "Deseleziona"
        if (s_editingExisting && s_selectedEntIdx >= 0) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f,0.15f,0.15f,0.9f));
            if (ImGui::SmallButton("X Deseleziona")) {
                s_hijackEntity = nullptr; s_previewActive = false;
                s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1;
            }
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // ── Lista scrollable ──
        ImGui::BeginChild("##entlist", ImVec2(-1.0f, -1.0f), false);

        int idx = 0;
        for (int i = 0; i < (int)g_ents.size(); i++) {
            auto& e = g_ents[i];

            // Applica filtri
            if (s_entNearOnly) {
                float dx=e.pos[0]-ppx, dz=e.pos[2]-ppz;
                if (sqrtf(dx*dx+dz*dz) > s_entNearRadius) { idx++; continue; }
            }
            if (s_entFilter[0]) {
                const char* nm = short_name(e.modelHandle);
                bool found = false;
                for (const char* p=nm; *p; p++) {
                    if (_strnicmp(p, s_entFilter, strlen(s_entFilter))==0) { found=true; break; }
                }
                if (!found) { idx++; continue; }
            }
            if (s_entTypeFilter > 0) {
                MapEntityType wanted = (MapEntityType)(s_entTypeFilter-1);
                if (e.type != wanted) { idx++; continue; }
            }

            bool selected = (s_editingExisting && s_selectedEntIdx == i);
            ImGui::PushID(i);

            // Colore tipo
            ImU32 tc = IM_COL32(160,160,160,255);
            switch (e.type) {
                case MapEntityType::Building: tc=IM_COL32(160,160,220,255); break;
                case MapEntityType::Shape:    tc=IM_COL32(200,150,100,255); break;
                case MapEntityType::Tree:     tc=IM_COL32( 50,200, 80,255); break;
                case MapEntityType::Grass:    tc=IM_COL32(100,220,100,255); break;
                case MapEntityType::Object:   tc=IM_COL32(100,150,255,255); break;
                default: break;
            }

            // Label: tipo + nome + pos
            char label[128];
            snprintf(label, sizeof(label), "[%s] %s\n  %.1f, %.1f, %.1f",
                type_str(e.type), short_name(e.modelHandle),
                e.pos[0], e.pos[1], e.pos[2]);

            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.22f,0.52f,0.78f,0.85f));

            if (ImGui::Selectable(label, selected, 0, ImVec2(0.0f, 30.0f))) {
                // Selezione: attiva gizmo sull'entità
                s_selectedEntIdx   = i;
                s_editingExisting  = true;
                s_hijackEntity     = e.ptr;
                s_previewPos[0]    = e.pos[0];
                s_previewPos[1]    = e.pos[1];
                s_previewPos[2]    = e.pos[2];
                s_previewActive        = true;
                s_selectedPlacementIdx = -1;
                s_gizmoMode            = 0; // Translate
            }

            if (selected) ImGui::PopStyleColor();
            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    // ── Save tab ──
    static void draw_save_tab()
    {
        int      N     = *s_mapN;
        uint16_t mapId = get_current_map_id();

        static uint16_t s_lastMapId = 0xFFFF;
        if (mapId != 0xFFFF && N > 0 && (s_pathSrc[0] == '\0' || mapId != s_lastMapId)) {
            build_wld_paths(s_pathSrc, sizeof(s_pathSrc), s_pathDst, sizeof(s_pathDst), mapId);
            s_lastMapId = mapId;
        }

        if (mapId != 0xFFFF && N > 0)
            ImGui::TextColored(ImVec4(0.4f,0.9f,1.0f,1.0f), "Mappa %u  (N=%d)", (unsigned)mapId, N);
        else
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Mappa non caricata");
        ImGui::Separator();

        ImGui::Text("Sorgente WLD:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##wsrc", s_pathSrc, sizeof(s_pathSrc), ImGuiInputTextFlags_ReadOnly);
        ImGui::Text("Output WLD:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##wdst", s_pathDst, sizeof(s_pathDst), ImGuiInputTextFlags_ReadOnly);
        ImGui::Separator();

        ImGui::Text("Confermati: %d  |  Entities: %d",
            (int)g_placements.size(), (int)g_ents.size());
        if (!g_placements.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Rimuovi tutti##ra")) {
                for (int i = (int)g_placements.size()-1; i >= 0; i--)
                    remove_placement(i);
            }
            for (int i = 0; i < (int)g_placements.size(); i++) {
                auto& h = g_placements[i];
                char btn[32]; snprintf(btn, sizeof(btn), "X##rm%d", i);
                if (ImGui::SmallButton(btn)) { remove_placement(i); i--; continue; }
                ImGui::SameLine();
                const char* fn = model_filename(h.newHandle);
                ImGui::Text("[%s] %s  (%.1f, %.1f, %.1f)",
                    type_str(h.type), fn ? fn : "?", h.pos[0], h.pos[1], h.pos[2]);
            }
        }
        ImGui::Separator();
        if (ImGui::Button("  Esporta WLD  "))
            do_export_wld();
        ImGui::Separator();
        ImVec4 mc = (strncmp(s_exportMsg,"OK",2)==0)
            ? ImVec4(0.2f,1.0f,0.5f,1.0f)
            : (strncmp(s_exportMsg,"ERRORE",6)==0 ? ImVec4(1,0.3f,0.3f,1) : ImVec4(0.7f,0.7f,0.7f,1));
        ImGui::TextColored(mc, "%s", s_exportMsg);

        if (s_lastSavedPath[0] != '\0' && strncmp(s_exportMsg,"OK",2)==0) {
            const char* fname = strrchr(s_lastSavedPath, '\\');
            fname = fname ? fname + 1 : s_lastSavedPath;
            char srcFname[64] = {};
            snprintf(srcFname, sizeof(srcFname), "%u.wld", (unsigned)mapId);
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.4f,1.0f), "Tip:");
            ImGui::TextWrapped("File: %s\nCopia in Data\\World\\ e rinomina in '%s'.",
                s_lastSavedPath, srcFname);
        }
    }

    // ── Sidebar (full-height, anchored left) ──
    static void draw_sidebar()
    {
        ImGuiIO& io = ImGui::GetIO();
        float screenH = io.DisplaySize.y;

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(kSidebarW, screenH));
        ImGui::SetNextWindowBgAlpha(0.97f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(4.0f, 5.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.11f, 0.97f));

        ImGui::Begin("##sidebar_me", nullptr,
            ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize     |
            ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar  |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

        // Title
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f,0.80f,0.30f,1.0f));
        ImGui::TextWrapped("MAP\nEDITOR");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        struct SBtn { const char* label; EditorSection sec; ImU32 base; ImU32 bright; };
        static const SBtn kBtns[] = {
            { "Build",  EditorSection::Build,  IM_COL32( 70, 70,155,215), IM_COL32(100,100,200,255) },
            { "Trees",  EditorSection::Trees,  IM_COL32( 30,120, 45,215), IM_COL32( 45,160, 65,255) },
            { "Grass",  EditorSection::Grass,  IM_COL32( 55,145, 60,215), IM_COL32( 75,190, 85,255) },
            { "Vani",   EditorSection::Vani,   IM_COL32( 40,110,105,215), IM_COL32( 55,150,140,255) },
            { "Props",  EditorSection::Props,  IM_COL32(148, 92, 40,215), IM_COL32(190,120, 55,255) },
            { "Wall",   EditorSection::Wall,   IM_COL32(100, 75, 30,215), IM_COL32(145,110, 45,255) },
            { "Others", EditorSection::Others, IM_COL32( 55, 88,180,215), IM_COL32( 75,118,225,255) },
        };

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        for (auto& btn : kBtns) {
            bool active = (s_section == btn.sec);
            ImVec4 bc = ImGui::ColorConvertU32ToFloat4(active ? btn.bright : btn.base);
            ImVec4 bh = ImGui::ColorConvertU32ToFloat4(btn.bright);
            ImGui::PushStyleColor(ImGuiCol_Button,        bc);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bh);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bh);
            if (active) {
                // Gold left indicator strip
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(p.x - 4.0f, p.y),
                    ImVec2(p.x - 1.0f, p.y + 33.0f),
                    IM_COL32(255,215,55,255));
            }
            if (ImGui::Button(btn.label, ImVec2(kSidebarW - 10.0f, 33.0f)))
                s_section = (s_section == btn.sec) ? EditorSection::None : btn.sec;
            ImGui::PopStyleColor(3);
        }
        ImGui::PopStyleVar(); // FrameRounding

        // Terra
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        {
            bool terraAct = (s_section == EditorSection::Terra);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,
                terraAct ? ImVec4(0.52f,0.36f,0.20f,1.00f) : ImVec4(0.28f,0.18f,0.10f,0.80f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f,0.36f,0.20f,1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f,0.48f,0.28f,1.00f));
            if (terraAct) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(p.x - 4.0f, p.y),
                    ImVec2(p.x - 1.0f, p.y + 33.0f),
                    IM_COL32(255,215,55,255));
            }
            if (ImGui::Button("Terra", ImVec2(kSidebarW - 10.0f, 33.0f))) {
                if (s_section == EditorSection::Terra) {
                    s_section = EditorSection::None;
                    terra_release_tex();
                } else {
                    s_section = EditorSection::Terra;
                    s_terraCanvasDirty = true;
                    s_terraMsg[0] = '\0';
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Modifica heightmap e texture\ndel terreno (brush 2D).\nSalva in Output_Maps\\.");
        }

        ImGui::Spacing();
        {
            bool scAct = (s_section == EditorSection::Scatter);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,
                scAct ? ImVec4(0.20f,0.52f,0.22f,1.0f) : ImVec4(0.10f,0.30f,0.12f,0.80f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f,0.60f,0.28f,1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f,0.70f,0.34f,1.0f));
            if (scAct) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(p.x-4.0f,p.y), ImVec2(p.x-1.0f,p.y+33.0f),
                    IM_COL32(255,215,55,255));
            }
            if (ImGui::Button("Scatter", ImVec2(kSidebarW - 10.0f, 33.0f))) {
                s_section = (s_section == EditorSection::Scatter)
                    ? EditorSection::None : EditorSection::Scatter;
                s_terraCanvasDirty = true; // forza refresh canvas tipo
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pianta modelli Grass/Fiori\nsolo sui terreni erbosi\ntramite pennello sulla minimappa.");
        }

        ImGui::Spacing();
        {
            bool mapAct = (s_section == EditorSection::Existing);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,
                mapAct ? ImVec4(0.22f,0.52f,0.68f,1.0f) : ImVec4(0.14f,0.32f,0.50f,0.80f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f,0.60f,0.78f,1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.34f,0.70f,0.90f,1.0f));
            if (ImGui::Button("Mappa", ImVec2(kSidebarW - 10.0f, 33.0f)))
                s_section = mapAct ? EditorSection::None : EditorSection::Existing;
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Modifica entita' gia'\npresenti sulla mappa\n(%d catturate)", (int)g_ents.size());
        }

        // Push Save button to bottom of sidebar
        float curY    = ImGui::GetCursorPosY();
        float targetY = screenH - 62.0f;
        if (targetY > curY) ImGui::SetCursorPosY(targetY);

        ImGui::Separator();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        bool saveAct = (s_section == EditorSection::Save);
        ImGui::PushStyleColor(ImGuiCol_Button,
            saveAct ? ImVec4(0.18f,0.68f,0.48f,1.0f) : ImVec4(0.10f,0.46f,0.32f,0.88f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f,0.68f,0.48f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f,0.80f,0.56f,1.0f));
        if (ImGui::Button("Save", ImVec2(kSidebarW - 10.0f, 33.0f)))
            s_section = saveAct ? EditorSection::None : EditorSection::Save;
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        if (!g_placements.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f,0.82f,0.22f,1.0f));
            ImGui::Text(" %d piaz.", (int)g_placements.size());
            ImGui::PopStyleColor();
        }

        ImGui::End();
        ImGui::PopStyleColor();  // WindowBg
        ImGui::PopStyleVar(3);   // WindowRounding, WindowBorderSize, ItemSpacing
    }

    // ── Content panel (attached right of sidebar) ──
    static void draw_content()
    {
        if (s_section == EditorSection::None) return;

        ImGuiIO& io = ImGui::GetIO();
        float screenH = io.DisplaySize.y;

        ImGui::SetNextWindowPos(ImVec2(kSidebarW, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(s_contentW, screenH));
        ImGui::SetNextWindowBgAlpha(0.95f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.5f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f,0.11f,0.15f,0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.26f,0.26f,0.40f,0.80f));

        ImGui::Begin("##content_me", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Avviso: cartella Data non estratta → texture/modelli non caricano.
        if (!data_folder_present()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.32f,0.10f,0.10f,0.95f));
            ImGui::BeginChild("##no_data_warn", ImVec2(0.0f, 92.0f), true);
            ImGui::TextColored(ImVec4(1.0f,0.55f,0.20f,1.0f),
                "[!] Cartella \"Data\" non trovata nel client.");
            ImGui::TextWrapped(
                "Il Map Editor legge texture e modelli da \"Data\\\" accanto al gioco. "
                "Sembra che il client abbia solo data.saf/sah non estratto: "
                "le sezioni Building/Trees/Grass/Vani/Props/Wall/Others, Terra e "
                "Scatter resteranno vuote.");
            ImGui::TextColored(ImVec4(0.85f,0.85f,0.55f,1.0f),
                "Estrai data.saf/sah in <client>\\Data\\ (SahEditor v2.22) e riavvia.");
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        if (s_section == EditorSection::Save)
            draw_save_tab();
        else if (s_section == EditorSection::Existing)
            draw_existing_entities();
        else if (s_section == EditorSection::Terra)
            draw_terra_content();
        else if (s_section == EditorSection::Scatter)
            draw_content_scatter();
        else
            draw_content_models(s_section);

        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        // ── Drag handle: ridimensiona il pannello trascinando il bordo destro ──
        static bool s_panelResizing = false;
        auto& io2 = ImGui::GetIO();
        float handleX = kSidebarW + s_contentW;
        bool hovH = fabsf(io2.MousePos.x - handleX) < 6.0f
                 && io2.MousePos.y >= 0.0f && io2.MousePos.y <= screenH;
        if (hovH || s_panelResizing)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        // Attiva resize SOLO su click esplicito sulla linea handle, mai per drag/scroll/gizmo
        if (hovH && io2.MouseClicked[0] && !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemHovered())
            s_panelResizing = true;
        if (!io2.MouseDown[0])
            s_panelResizing = false;
        if (s_panelResizing)
            s_contentW = max(180.0f, min(900.0f, s_contentW + io2.MouseDelta.x));
        ImDrawList* hdl = ImGui::GetForegroundDrawList();
        ImU32 hc = (hovH||s_panelResizing) ? IM_COL32(160,160,220,230) : IM_COL32(80,80,120,100);
        hdl->AddLine({handleX,0.0f},{handleX,screenH}, hc, (hovH||s_panelResizing)?3.0f:1.5f);
    }

    // Ritorna true se handle è un muro (filename contiene "wall", case-insensitive)
    static bool is_wall_model(void* handle)
    {
        const char* fn = model_filename(handle);
        if (!fn) return false;
        for (const char* p = fn; p[0] && p[1] && p[2] && p[3]; p++) {
            if ((p[0]=='W'||p[0]=='w') && (p[1]=='A'||p[1]=='a') &&
                (p[2]=='L'||p[2]=='l') && (p[3]=='L'||p[3]=='l'))
                return true;
        }
        return false;
    }

    // Rodrigues: ruota i vettori fwd/up dell'entity hijackata attorno ad asse (ax,ay,az) di theta rad.
    static void entity_rotate_y(float axX, float axY, float axZ, float theta)
    {
        if (!s_hijackEntity) return;
        auto* b2 = (uint8_t*)s_hijackEntity;
        float f[3]={*(float*)(b2+0x10),*(float*)(b2+0x14),*(float*)(b2+0x18)};
        float u[3]={*(float*)(b2+0x1c),*(float*)(b2+0x20),*(float*)(b2+0x24)};
        auto rod=[](float v[3],float kx,float ky,float kz,float th){
            float co=cosf(th),si=sinf(th);
            float dt=v[0]*kx+v[1]*ky+v[2]*kz;
            float cx=ky*v[2]-kz*v[1], cy=kz*v[0]-kx*v[2], cz=kx*v[1]-ky*v[0];
            v[0]=v[0]*co+cx*si+kx*dt*(1.0f-co);
            v[1]=v[1]*co+cy*si+ky*dt*(1.0f-co);
            v[2]=v[2]*co+cz*si+kz*dt*(1.0f-co);
        };
        rod(f,axX,axY,axZ,theta);
        rod(u,axX,axY,axZ,theta);
        *(float*)(b2+0x10)=f[0];*(float*)(b2+0x14)=f[1];*(float*)(b2+0x18)=f[2];
        *(float*)(b2+0x1c)=u[0];*(float*)(b2+0x20)=u[1];*(float*)(b2+0x24)=u[2];
        s_UpdateMatrix(s_hijackEntity);
    }

    // ── Gizmo 3D (stile Unity) ──
    // Modalità T (Trasla): frecce X/Y/Z + cerchio XZ. Click cerchio → passa a R.
    // Modalità R (Ruota): 3 anelli X/Y/Z (Rodrigues). Click cerchio → passa a T.
    static void draw_gizmos()
    {
        if (!s_previewActive || !s_hijackEntity) return;

        IDirect3DDevice9* pD3D = *ppD3DDevice;
        if (!pD3D) return;

        D3DMATRIX    matView, matProj;
        D3DVIEWPORT9 vp;
        pD3D->GetTransform(D3DTS_VIEW,       &matView);
        pD3D->GetTransform(D3DTS_PROJECTION, &matProj);
        pD3D->GetViewport(&vp);

        float wx = s_previewPos[0], wy = s_previewPos[1], wz = s_previewPos[2];

        ImVec2 center;
        if (!world_to_screen(wx, wy, wz, matView, matProj, vp, center)) return;

        float viewZ = wx*matView._13 + wy*matView._23 + wz*matView._33 + matView._43;
        float pxPerUnit = (viewZ > 0.5f)
            ? (vp.Width * 0.5f) * fabsf(matProj._11) / viewZ : 20.0f;
        float AL = max(1.5f, min(80.0f / pxPerUnit, 30.0f));

        ImDrawList* dl  = ImGui::GetForegroundDrawList();
        ImVec2      mp  = ImGui::GetIO().MousePos;
        ImVec2      mdt = ImGui::GetIO().MouseDelta;
        bool        mdn = ImGui::GetIO().MouseDown[0];
        bool        mcl = ImGui::GetIO().MouseClicked[0];

        static int   s_axis          = -1;
        static bool  s_dragging      = false;
        static float s_trailStart[3] = {}; // posizione all'inizio del drag XZ

        auto lineD = [](ImVec2 p, ImVec2 a, ImVec2 b) -> float {
            float dx=b.x-a.x,dy=b.y-a.y,l2=dx*dx+dy*dy;
            if(l2<0.001f){float ex=p.x-a.x,ey=p.y-a.y;return sqrtf(ex*ex+ey*ey);}
            float t=((p.x-a.x)*dx+(p.y-a.y)*dy)/l2; if(t<0)t=0;if(t>1)t=1;
            float fx=a.x+t*dx-p.x,fy=a.y+t*dy-p.y; return sqrtf(fx*fx+fy*fy);
        };
        auto ptD = [](ImVec2 a, ImVec2 b) -> float {
            float dx=a.x-b.x,dy=a.y-b.y; return sqrtf(dx*dx+dy*dy);
        };
        auto gc = [](bool h, ImU32 base, ImU32 br) -> ImU32 { return h ? br : base; };

        // applyRot definita come funzione statica di namespace — vedi sotto draw_gizmos
        auto applyRot = [](float axX, float axY, float axZ, float theta) {
            entity_rotate_y(axX, axY, axZ, theta);
        };

        bool hovC = !s_dragging && ptD(mp, center) < 9.0f;

        if (s_gizmoMode == 0) {
            // ── MODALITA' TRASLA ──
            ImVec2 tipX, tipY, tipZ;
            bool visX = world_to_screen(wx+AL, wy,    wz,    matView, matProj, vp, tipX);
            bool visY = world_to_screen(wx,    wy+AL, wz,    matView, matProj, vp, tipY);
            bool visZ = world_to_screen(wx,    wy,    wz+AL, matView, matProj, vp, tipZ);

            const float SQ = 9.0f; // mezzo lato dei quadratini handle
            auto inSq = [&](ImVec2 t) -> bool {
                return fabsf(mp.x-t.x) <= SQ+2 && fabsf(mp.y-t.y) <= SQ+2;
            };
            bool hovX = !s_dragging && !hovC && visX && (lineD(mp,center,tipX)<8.0f || inSq(tipX));
            bool hovY = !s_dragging && !hovC && !hovX && visY && (lineD(mp,center,tipY)<8.0f || inSq(tipY));
            bool hovZ = !s_dragging && !hovC && !hovX && !hovY && visZ && (lineD(mp,center,tipZ)<8.0f || inSq(tipZ));
            bool anyH = hovC||hovX||hovY||hovZ;

            const float LW = 3.0f;
            if (visX) dl->AddLine(center,tipX, gc(hovX||s_axis==0, IM_COL32(185,38,38,200),IM_COL32(255,75,75,255)), LW);
            if (visY) dl->AddLine(center,tipY, gc(hovY||s_axis==1, IM_COL32(38,185,38,200),IM_COL32(75,255,75,255)), LW);
            if (visZ) dl->AddLine(center,tipZ, gc(hovZ||s_axis==2, IM_COL32(38,38,185,200),IM_COL32(75,75,255,255)), LW);

            // Quadratini handle alle punte (più facili da cliccare del triangolo)
            auto drawSqHandle = [&](ImVec2 tip, bool hov, bool drag,
                                    ImU32 base, ImU32 bright, ImU32 border) {
                ImU32 fill = (hov||drag) ? bright : base;
                dl->AddRectFilled({tip.x-SQ,tip.y-SQ},{tip.x+SQ,tip.y+SQ}, fill, 2.5f);
                dl->AddRect      ({tip.x-SQ,tip.y-SQ},{tip.x+SQ,tip.y+SQ}, border, 2.5f, 0, 1.5f);
            };
            if (visX) drawSqHandle(tipX, hovX, s_axis==0,
                IM_COL32(185,38,38,210), IM_COL32(255,90,90,255), IM_COL32(255,160,160,200));
            if (visY) drawSqHandle(tipY, hovY, s_axis==1,
                IM_COL32(38,185,38,210), IM_COL32(90,255,90,255), IM_COL32(160,255,160,200));
            if (visZ) drawSqHandle(tipZ, hovZ, s_axis==2,
                IM_COL32(38,38,185,210), IM_COL32(90,90,255,255), IM_COL32(160,160,255,200));

            if (visX) dl->AddText({tipX.x+SQ+3,tipX.y-6}, IM_COL32(255,90,90,255),  "X");
            if (visY) dl->AddText({tipY.x+SQ+3,tipY.y-6}, IM_COL32(90,255,90,255),  "Y");
            if (visZ) dl->AddText({tipZ.x+SQ+3,tipZ.y-6}, IM_COL32(90,90,255,255),  "Z");

            // Cerchio centrale "T" (click → passa a Ruota)
            dl->AddCircleFilled(center, 7.0f,
                (hovC||s_axis==3) ? IM_COL32(255,255,180,255) : IM_COL32(210,210,160,215));
            dl->AddCircle(center, 7.0f, IM_COL32(60,60,60,200), 16, 1.5f);
            dl->AddText({center.x-3,center.y-5}, IM_COL32(30,30,30,255), "T");

            if (anyH && !s_dragging) {
                bool ctrl = ImGui::GetIO().KeyCtrl;
                const char* hint = hovC  ? (ctrl ? "[Ctrl+Click: modalita' Ruota]" : "Trascina: muovi XZ") :
                                   hovX  ? "Trascina: muovi X" :
                                   hovY  ? "Trascina: muovi Y" : "Trascina: muovi Z";
                dl->AddText({center.x+11,center.y+6}, IM_COL32(200,200,200,200), hint);
            }

            if (!s_dragging && mcl && anyH) {
                if (hovC) {
                    if (ImGui::GetIO().KeyCtrl) { s_gizmoMode = 1; s_axis = -1; }
                    else {
                        s_trailStart[0]=s_previewPos[0];
                        s_trailStart[1]=s_previewPos[1];
                        s_trailStart[2]=s_previewPos[2];
                        s_axis = 3; s_dragging = true;
                    }
                } else {
                    s_axis = hovX?0 : hovY?1 : 2; s_dragging = true;
                }
            }

            // Trail line: linea dal punto di partenza alla posizione corrente durante drag XZ
            if (s_dragging && s_axis == 3) {
                ImVec2 startScrn;
                if (world_to_screen(s_trailStart[0],s_trailStart[1],s_trailStart[2],matView,matProj,vp,startScrn)) {
                    dl->AddLine(startScrn, center, IM_COL32(255,240,80,170), 1.5f);
                    dl->AddCircleFilled(startScrn, 5.0f, IM_COL32(255,230,60,220));
                    dl->AddCircle(startScrn, 5.0f, IM_COL32(200,180,30,255), 12, 1.5f);
                    float wdx=s_previewPos[0]-s_trailStart[0], wdz=s_previewPos[2]-s_trailStart[2];
                    float dist=sqrtf(wdx*wdx+wdz*wdz);
                    char db[32]; snprintf(db,32,"%.1f u",dist);
                    dl->AddText({(startScrn.x+center.x)*0.5f+3,(startScrn.y+center.y)*0.5f-14},
                        IM_COL32(255,245,120,230), db);
                    // Snap indicator
                    if (g_isSnapping) {
                        ImVec2 snapScrn;
                        if (world_to_screen(g_snapPtX, g_snapPtY, g_snapPtZ, matView, matProj, vp, snapScrn)) {
                            dl->AddCircle(snapScrn, 10.0f, IM_COL32(50,255,80,230), 16, 2.0f);
                            dl->AddCircleFilled(snapScrn, 4.0f, IM_COL32(50,255,80,255));
                            dl->AddLine(center, snapScrn, IM_COL32(50,255,80,130), 1.2f);
                            dl->AddText({snapScrn.x+12, snapScrn.y-7}, IM_COL32(50,255,80,230), "SNAP");
                        }
                    }
                }
            }

            if (s_dragging && (mdt.x!=0.0f||mdt.y!=0.0f)) {
                auto axisMov = [&](int ax) -> float {
                    float ox=wx,oy=wy,oz=wz;
                    if(ax==0)ox+=1.0f; else if(ax==1)oy+=1.0f; else oz+=1.0f;
                    ImVec2 ap;
                    if(!world_to_screen(ox,oy,oz,matView,matProj,vp,ap)) return 0.0f;
                    float sdx=ap.x-center.x,sdy=ap.y-center.y,l2=sdx*sdx+sdy*sdy;
                    if(l2<0.1f) return 0.0f;
                    return (mdt.x*sdx+mdt.y*sdy)/l2;
                };
                float dx=0,dy=0,dz=0;
                if(s_axis==0) dx=axisMov(0);
                else if(s_axis==1) dy=axisMov(1);
                else if(s_axis==2) dz=axisMov(2);
                else { dx=axisMov(0); dz=axisMov(2); }
                s_previewPos[0]+=dx; s_previewPos[1]+=dy; s_previewPos[2]+=dz;
                // SHIFT: snap al metro (grid 1.0 unit)
                if (ImGui::GetIO().KeyShift) {
                    s_previewPos[0] = roundf(s_previewPos[0]);
                    s_previewPos[2] = roundf(s_previewPos[2]);
                }
                // ── Wall edge snap (drag XZ, world-axis-aligned, su modelli Wall) ──
                // Usa AABB reale del modello per calcolare distanze edge-to-edge su assi X e Z.
                // Nessuna dipendenza dal vettore fwd — funziona per muri a 0° e 90°.
                // Allinea anche Y (altezza) alla parete di riferimento.
                if (s_axis == 3) {
                    auto* b_e = (uint8_t*)s_hijackEntity;
                    void* curHandle = *(void**)(b_e + 0xa0);
                    g_isSnapping = false;
                    if (is_wall_model(curHandle)) {
                        // Metà-estensioni del muro corrente su assi mondo X e Z
                        float curAabb[6];
                        bool  curOk = get_model_aabb(curHandle, curAabb);
                        float curHX = curOk ? (curAabb[3]-curAabb[0])*0.5f : g_wallSnapWidth*0.5f;
                        float curHZ = curOk ? (curAabb[5]-curAabb[2])*0.5f : g_wallSnapDepth*0.5f;

                        float bestDist = g_wallSnapThreshold;
                        float bestX = s_previewPos[0], bestY = s_previewPos[1], bestZ = s_previewPos[2];

                        // Lambda: snap 4 punti usando assi LOCALI del muro di riferimento.
                        // rfwd/rup = rotazione in world space del muro ref.
                        // Offset lungo right (cross rup×rfwd) = fianco-a-fianco.
                        // Offset lungo rfwd (profondità) = testa-a-testa.
                        auto check_wall = [&](float px, float py, float pz,
                                               const float* rfwd, const float* rup,
                                               void* refHandle) {
                            float refAabb[6];
                            bool  refOk = get_model_aabb(refHandle, refAabb);
                            float refHX = refOk ? (refAabb[3]-refAabb[0])*0.5f : g_wallSnapWidth*0.5f;
                            float refHZ = refOk ? (refAabb[5]-refAabb[2])*0.5f : g_wallSnapDepth*0.5f;

                            // Vettore "right" del muro ref = cross(rup, rfwd), normalizzato
                            float rr[3] = {
                                rup[1]*rfwd[2] - rup[2]*rfwd[1],
                                rup[2]*rfwd[0] - rup[0]*rfwd[2],
                                rup[0]*rfwd[1] - rup[1]*rfwd[0]
                            };
                            float rl = sqrtf(rr[0]*rr[0] + rr[1]*rr[1] + rr[2]*rr[2]);
                            if (rl > 0.001f) { rr[0]/=rl; rr[1]/=rl; rr[2]/=rl; }

                            // rfwd normalizzato in XZ (ignora Y per snap orizzontale)
                            float fl = sqrtf(rfwd[0]*rfwd[0] + rfwd[2]*rfwd[2]);
                            float rfx = fl > 0.001f ? rfwd[0]/fl : 0.0f;
                            float rfz = fl > 0.001f ? rfwd[2]/fl : 1.0f;

                            // 4 candidati:
                            //   [0,1] fianco-a-fianco: ±right * (refHX + curHX)
                            //   [2,3] testa-a-testa:   ±fwd   * (refHZ + curHZ)
                            float dW = refHX + curHX;
                            float dD = refHZ + curHZ;
                            float cx[4] = { px+rr[0]*dW, px-rr[0]*dW, px+rfx*dD, px-rfx*dD };
                            float cz[4] = { pz+rr[2]*dW, pz-rr[2]*dW, pz+rfz*dD, pz-rfz*dD };
                            for (int k = 0; k < 4; k++) {
                                float ddx=s_previewPos[0]-cx[k], ddz=s_previewPos[2]-cz[k];
                                float d=sqrtf(ddx*ddx+ddz*ddz);
                                if (d < bestDist) {
                                    bestDist=d; bestX=cx[k]; bestY=py; bestZ=cz[k];
                                    g_snapPtX=cx[k]; g_snapPtY=py; g_snapPtZ=cz[k];
                                    g_isSnapping=true;
                                }
                            }
                        };

                        for (auto& e : g_ents)
                            if (is_wall_model(e.modelHandle))
                                check_wall(e.pos[0], e.pos[1], e.pos[2], e.rot, e.rotUp, e.modelHandle);

                        for (int pi = 0; pi < (int)g_placements.size(); pi++) {
                            if (pi == s_selectedPlacementIdx) continue;
                            auto& h = g_placements[pi];
                            if (is_wall_model(h.newHandle))
                                check_wall(h.pos[0], h.pos[1], h.pos[2], h.rot, h.rotUp, h.newHandle);
                        }

                        if (g_isSnapping) {
                            s_previewPos[0]=bestX;
                            s_previewPos[1]=bestY; // allinea altezza
                            s_previewPos[2]=bestZ;
                        }
                    }
                }
                auto* b2=(uint8_t*)s_hijackEntity;
                *(float*)(b2+0x04)=s_previewPos[0];
                *(float*)(b2+0x08)=s_previewPos[1];
                *(float*)(b2+0x0c)=s_previewPos[2];
                s_UpdateMatrix(s_hijackEntity);
                if (s_selectedPlacementIdx >= 0 && s_selectedPlacementIdx < (int)g_placements.size())
                    memcpy(g_placements[s_selectedPlacementIdx].pos, s_previewPos, 12);
                if (s_editingExisting && s_selectedEntIdx >= 0 && s_selectedEntIdx < (int)g_ents.size())
                    memcpy(g_ents[s_selectedEntIdx].pos, s_previewPos, 12);
            }
        } else {
            // ── MODALITA' RUOTA ──
            const int   NPTS = 36;
            const float RR   = AL * 1.3f;    // raggio anelli (world units)
            const float PI2  = 6.28318f;

            // Campiona un anello 3D su piano definito da (d1,d2) e proietta a schermo
            auto buildRing = [&](float d1x,float d1y,float d1z,
                                  float d2x,float d2y,float d2z) -> std::vector<ImVec2> {
                std::vector<ImVec2> pts;
                pts.reserve(NPTS+1);
                for (int k = 0; k <= NPTS; k++) {
                    float a = k*PI2/NPTS, cosA=cosf(a), sinA=sinf(a);
                    float rx=wx+RR*(cosA*d1x+sinA*d2x);
                    float ry=wy+RR*(cosA*d1y+sinA*d2y);
                    float rz=wz+RR*(cosA*d1z+sinA*d2z);
                    ImVec2 sp;
                    if (world_to_screen(rx,ry,rz,matView,matProj,vp,sp))
                        pts.push_back(sp);
                }
                return pts;
            };

            // Y-ring (piano XZ, verde = yaw)
            auto ringY = buildRing(1,0,0, 0,0,1);
            // X-ring (piano YZ, rosso = pitch)
            auto ringX = buildRing(0,1,0, 0,0,1);
            // Z-ring (piano XY, blu = roll)
            auto ringZ = buildRing(1,0,0, 0,1,0);

            auto ringMinD = [&](const std::vector<ImVec2>& ring) -> float {
                float minD=1e9f;
                for (int k=0;k+1<(int)ring.size();k++)
                    minD=min(minD, lineD(mp,ring[k],ring[k+1]));
                return minD;
            };

            bool hovRY = !s_dragging && !hovC && (int)ringY.size()>1 && ringMinD(ringY)<12.0f;
            bool hovRX = !s_dragging && !hovC && !hovRY && (int)ringX.size()>1 && ringMinD(ringX)<12.0f;
            bool hovRZ = !s_dragging && !hovC && !hovRY && !hovRX && (int)ringZ.size()>1 && ringMinD(ringZ)<12.0f;
            bool anyH2 = hovC||hovRY||hovRX||hovRZ;

            auto drawRing = [&](const std::vector<ImVec2>& ring, bool hov, int ax, ImU32 base, ImU32 bright) {
                if ((int)ring.size()>1)
                    dl->AddPolyline(ring.data(),(int)ring.size(),
                        (hov||s_axis==ax)?bright:base, 0, 2.5f);
            };
            drawRing(ringY, hovRY, 1, IM_COL32(38,160,38,190),  IM_COL32(80,255,80,255));
            drawRing(ringX, hovRX, 0, IM_COL32(160,38,38,190),  IM_COL32(255,80,80,255));
            drawRing(ringZ, hovRZ, 2, IM_COL32(38,38,160,190),  IM_COL32(80,80,255,255));

            // Etichette anelli (al primo quarto del campionamento)
            auto ringLabelPt = [](const std::vector<ImVec2>& r) -> ImVec2 {
                int idx = (int)r.size()/4; return idx < (int)r.size() ? r[idx] : ImVec2{0,0};
            };
            if (!ringY.empty()){auto p=ringLabelPt(ringY); dl->AddText({p.x+3,p.y-8},IM_COL32(80,255,80,255),"Y");}
            if (!ringX.empty()){auto p=ringLabelPt(ringX); dl->AddText({p.x+3,p.y-8},IM_COL32(255,80,80,255),"X");}
            if (!ringZ.empty()){auto p=ringLabelPt(ringZ); dl->AddText({p.x+3,p.y-8},IM_COL32(80,80,255,255),"Z");}

            // Cerchio centrale "R" (click → passa a Trasla)
            dl->AddCircleFilled(center, 7.0f,
                hovC ? IM_COL32(255,255,180,255) : IM_COL32(210,210,160,215));
            dl->AddCircle(center, 7.0f, IM_COL32(60,60,60,200), 16, 1.5f);
            dl->AddText({center.x-3,center.y-5}, IM_COL32(30,30,30,255), "R");

            if (anyH2 && !s_dragging) {
                bool ctrl = ImGui::GetIO().KeyCtrl;
                const char* hint = hovC  ? (ctrl ? "[Ctrl+Click: modalita' Sposta]" : "Ctrl+Click: torna a Sposta") :
                                   hovRY ? "Trascina: ruota Y (yaw)"   :
                                   hovRX ? "Trascina: ruota X (pitch)"  : "Trascina: ruota Z (roll)";
                dl->AddText({center.x+11,center.y+6}, IM_COL32(200,200,200,200), hint);
            }

            if (!s_dragging && mcl && anyH2) {
                if (hovC && ImGui::GetIO().KeyCtrl) { s_gizmoMode = 0; s_axis = -1; }
                else if (!hovC)                      { s_axis = hovRX?0 : hovRY?1 : 2; s_dragging = true; }
            }

            if (s_dragging && (mdt.x!=0.0f||mdt.y!=0.0f)) {
                const float SPD = 0.012f; // radianti per pixel
                if      (s_axis==1) applyRot(0.0f,1.0f,0.0f,  mdt.x*SPD); // Y yaw
                else if (s_axis==0) applyRot(1.0f,0.0f,0.0f,  mdt.y*SPD); // X pitch
                else if (s_axis==2) applyRot(0.0f,0.0f,1.0f,  mdt.x*SPD); // Z roll
                if (s_selectedPlacementIdx >= 0 && s_selectedPlacementIdx < (int)g_placements.size()) {
                    auto& hp = g_placements[s_selectedPlacementIdx];
                    auto* b3 = (uint8_t*)s_hijackEntity;
                    hp.rot[0]  = *(float*)(b3+0x10); hp.rot[1]  = *(float*)(b3+0x14); hp.rot[2]  = *(float*)(b3+0x18);
                    hp.rotUp[0]= *(float*)(b3+0x1c); hp.rotUp[1]= *(float*)(b3+0x20); hp.rotUp[2]= *(float*)(b3+0x24);
                }
                if (s_editingExisting && s_selectedEntIdx >= 0 && s_selectedEntIdx < (int)g_ents.size()) {
                    auto* b3 = (uint8_t*)s_hijackEntity;
                    auto& ge = g_ents[s_selectedEntIdx];
                    ge.rot[0]  = *(float*)(b3+0x10); ge.rot[1]  = *(float*)(b3+0x14); ge.rot[2]  = *(float*)(b3+0x18);
                    ge.rotUp[0]= *(float*)(b3+0x1c); ge.rotUp[1]= *(float*)(b3+0x20); ge.rotUp[2]= *(float*)(b3+0x24);
                }
            }
        }

        // Coordinate correnti (entrambe le modalità)
        char buf[64]; snprintf(buf,64,"%.1f  %.1f  %.1f",wx,wy,wz);
        dl->AddText({center.x+11,center.y-8}, IM_COL32(230,230,230,210), buf);

        if (!mdn) { s_dragging=false; s_axis=-1; }
    }

    // ── Inspector: finestra floating con info + controlli del placement selezionato ──
    static void draw_inspector_panel()
    {
        if (!s_previewActive || !s_hijackEntity) return;

        ImGuiIO& io = ImGui::GetIO();
        float panelRight = kSidebarW + (s_section != EditorSection::None ? s_contentW : 0.0f);

        ImGui::SetNextWindowPos(ImVec2(panelRight + 8.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(230.0f, 0.0f), ImVec2(320.0f, 500.0f));
        ImGui::SetNextWindowBgAlpha(0.93f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f,0.09f,0.13f,0.93f));
        ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.36f,0.36f,0.56f,0.80f));

        ImGui::Begin("##inspector_me", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header — differentzia tra entità esistente e placement nuovo
        if (s_editingExisting) {
            ImGui::TextColored(ImVec4(0.40f,0.80f,1.00f,1.0f), "Inspector [Mappa]");
        } else {
            ImGui::TextColored(ImVec4(0.95f,0.80f,0.30f,1.0f), "Inspector");
        }
        bool hasSel = (s_selectedPlacementIdx >= 0 && s_selectedPlacementIdx < (int)g_placements.size());
        bool hasExisting = (s_editingExisting && s_selectedEntIdx >= 0 && s_selectedEntIdx < (int)g_ents.size());
        if (hasSel) {
            ImGui::SameLine();
            ImGui::TextDisabled(" #%d / %d", s_selectedPlacementIdx+1, (int)g_placements.size());
        } else if (hasExisting) {
            ImGui::SameLine();
            ImGui::TextDisabled(" ent #%d", s_selectedEntIdx);
        }
        ImGui::Separator();

        // Info modello
        void* infoHandle = nullptr;
        if (hasSel) {
            auto& h = g_placements[s_selectedPlacementIdx];
            ImGui::Text("Category: %s", type_str_full(h.type));
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::TextDisabled("TypeID: %d", h.modelIndex);
            infoHandle = h.newHandle;
        } else if (hasExisting) {
            auto& e = g_ents[s_selectedEntIdx];
            ImGui::TextColored(ImVec4(0.40f,0.80f,1.00f,1.0f), "Tipo: %s", type_str_full(e.type));
            ImGui::TextDisabled("Pos orig: %.1f  %.1f  %.1f", e.pos[0], e.pos[1], e.pos[2]);
            infoHandle = e.modelHandle;
        } else {
            ImGui::TextDisabled("(preview libera)");
        }
        if (infoHandle) {
            const char* fn = model_filename(infoHandle);
            if (fn) {
                const char* base = strrchr(fn, '/');
                ImGui::TextDisabled("%s", base ? base+1 : fn);
            }
            // AABB del modello
            float aabb[6];
            if (get_model_aabb(infoHandle, aabb)) {
                float sX = aabb[3]-aabb[0], sY = aabb[4]-aabb[1], sZ = aabb[5]-aabb[2];
                ImGui::TextColored(ImVec4(0.5f,0.9f,0.5f,1.0f),
                    "Dim: %.2f x %.2f x %.2f", sX, sY, sZ);
                ImGui::SameLine();
                ImGui::TextDisabled("(X Y Z)");
            } else {
                ImGui::TextDisabled("AABB: n/d");
            }
        }

        ImGui::Separator();

        // Gizmo Mode (Toggle Translate / Rotate)
        ImGui::TextDisabled("Gizmo Mode:");
        ImGui::SameLine();
        {
            bool isT = (s_gizmoMode == 0);
            ImGui::PushStyleColor(ImGuiCol_Button, isT
                ? ImVec4(0.22f,0.45f,0.82f,0.95f) : ImVec4(0.18f,0.18f,0.28f,0.80f));
            if (ImGui::SmallButton("Translate")) s_gizmoMode = 0;
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        {
            bool isR = (s_gizmoMode == 1);
            ImGui::PushStyleColor(ImGuiCol_Button, isR
                ? ImVec4(0.22f,0.45f,0.82f,0.95f) : ImVec4(0.18f,0.18f,0.28f,0.80f));
            if (ImGui::SmallButton("Rotate")) s_gizmoMode = 1;
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // Position (DragFloat per asse)
        ImGui::Text("Position:");
        float fw = max(60.0f, (ImGui::GetContentRegionAvail().x - 10.0f) / 3.0f);
        bool moved = false;
        ImGui::PushItemWidth(fw);
        moved |= ImGui::DragFloat("X##ix", &s_previewPos[0], 0.5f);
        ImGui::SameLine(0.0f, 4.0f);
        moved |= ImGui::DragFloat("Y##iy", &s_previewPos[1], 0.5f);
        ImGui::SameLine(0.0f, 4.0f);
        moved |= ImGui::DragFloat("Z##iz", &s_previewPos[2], 0.5f);
        ImGui::PopItemWidth();
        if (moved && s_hijackEntity) {
            auto* b = (uint8_t*)s_hijackEntity;
            *(float*)(b+0x04) = s_previewPos[0];
            *(float*)(b+0x08) = s_previewPos[1];
            *(float*)(b+0x0c) = s_previewPos[2];
            s_UpdateMatrix(s_hijackEntity);
            if (hasSel) memcpy(g_placements[s_selectedPlacementIdx].pos, s_previewPos, 12);
        }

        // Reset Rotation — riporta alla rotazione di piazzamento (fwd/up iniziali)
        if (hasSel && s_hijackEntity) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f,0.42f,0.58f,0.88f));
            if (ImGui::Button("Reset Rot##rr", ImVec2(-1.0f, 0.0f))) {
                auto& hp = g_placements[s_selectedPlacementIdx];
                auto* b  = (uint8_t*)s_hijackEntity;
                *(float*)(b+0x10) = hp.initRot[0]; *(float*)(b+0x14) = hp.initRot[1]; *(float*)(b+0x18) = hp.initRot[2];
                *(float*)(b+0x1c) = hp.initUp[0];  *(float*)(b+0x20) = hp.initUp[1];  *(float*)(b+0x24) = hp.initUp[2];
                s_UpdateMatrix(s_hijackEntity);
                memcpy(hp.rot,   hp.initRot, 12);
                memcpy(hp.rotUp, hp.initUp,  12);
            }
            ImGui::PopStyleColor();
        }

        // ── Preset rotazioni Y (chiave per muri) ──
        ImGui::TextDisabled("Rot Y:");
        {
            struct { const char* lbl; float deg; } presets[] = {
                {"+45",45},{"90",90},{"-45",-45},{"-90",-90},{"180",180}
            };
            float pw = (ImGui::GetContentRegionAvail().x - 8.0f) / 5.0f;
            for (int pi2 = 0; pi2 < 5; pi2++) {
                if (pi2 > 0) ImGui::SameLine(0, 2);
                if (ImGui::Button(presets[pi2].lbl, ImVec2(pw, 0.0f))) {
                    float theta = presets[pi2].deg * (3.14159265f / 180.0f);
                    entity_rotate_y(0.0f, 1.0f, 0.0f, theta);
                    // Sincronizza rot nel placement se esiste
                    if (hasSel) {
                        auto& hp2 = g_placements[s_selectedPlacementIdx];
                        auto* bh  = (uint8_t*)s_hijackEntity;
                        hp2.rot[0]=*(float*)(bh+0x10); hp2.rot[1]=*(float*)(bh+0x14); hp2.rot[2]=*(float*)(bh+0x18);
                        hp2.rotUp[0]=*(float*)(bh+0x1c); hp2.rotUp[1]=*(float*)(bh+0x20); hp2.rotUp[2]=*(float*)(bh+0x24);
                    }
                }
            }
        }

        // Wall snap sliders — label sopra, slider full-width senza testo laterale
        ImGui::Separator();
        // Mostra se AABB auto è disponibile per il modello selezionato
        {
            bool hasAabb = false;
            if (hasSel) {
                auto& h2 = g_placements[s_selectedPlacementIdx];
                float tmp[6];
                hasAabb = get_model_aabb(h2.newHandle, tmp);
            }
            if (hasAabb)
                ImGui::TextColored(ImVec4(0.3f,1.0f,0.4f,1.0f), "Wall Snap [auto AABB]");
            else
                ImGui::TextDisabled("Wall Snap [fallback]");
        }
        ImGui::TextDisabled("Fallback lun:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##wsw", &g_wallSnapWidth, 1.0f, 40.0f, "%.1f u");
        ImGui::TextDisabled("Fallback sp: "); ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##wsd", &g_wallSnapDepth, 0.1f, 10.0f, "%.1f u");
        ImGui::TextDisabled("Soglia:      "); ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##wsr", &g_wallSnapThreshold, 0.2f, 8.0f, "%.1f u");

        ImGui::Separator();

        // Undo (ultimo piazzato) + Delete Selected
        float bw = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;

        bool canUndo = !g_placements.empty();
        if (!canUndo) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.52f,0.38f,0.10f,0.85f));
        if (ImGui::Button("Undo##ui", ImVec2(bw, 0.0f))) {
            int last = (int)g_placements.size()-1;
            if (s_selectedPlacementIdx == last) {
                s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1;
            }
            remove_placement(last);
            if (s_selectedPlacementIdx >= (int)g_placements.size()) {
                int ns = (int)g_placements.size() - 1;
                if (ns >= 0) select_placement(ns);
                else { s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1; }
            }
        }
        ImGui::PopStyleColor();
        if (!canUndo) ImGui::EndDisabled();

        ImGui::SameLine(0.0f, 4.0f);

        if (!hasSel) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f,0.12f,0.12f,0.85f));
        if (ImGui::Button("Delete Selected##di", ImVec2(bw, 0.0f))) {
            int idx = s_selectedPlacementIdx;
            s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1;
            remove_placement(idx);
            if (!g_placements.empty()) {
                int ns = min(idx, (int)g_placements.size()-1);
                select_placement(ns);
            }
        }
        ImGui::PopStyleColor();
        if (!hasSel) ImGui::EndDisabled();

        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void draw_map_editor_panel()
    {
        // Gate admin come gli altri pannelli GM (debug_panel F9, custom_gm_panel).
        // Non-admin: F5 non togglea e il pannello resta forzato chiuso.
        const bool isAdmin = g_pPlayerData && g_pPlayerData->isAdmin;
        if (!isAdmin) {
            g_showMapEditor = false;
            return;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
            g_showMapEditor = !g_showMapEditor;

        if (!g_showMapEditor) return;

        // Rilevatore cambio mappa — resetta minimap, canvas, paths display
        {
            static uint16_t s_panelLastMapId = 0xFFFF;
            uint16_t curMapId = get_current_map_id();
            if (curMapId != 0xFFFF && curMapId != s_panelLastMapId) {
                s_panelLastMapId   = curMapId;
                s_terraMiniMapId   = 0xFFFF;   // forza reload minimap Terra
                s_terraCanvasDirty = true;      // forza refresh canvas Scatter/Terra
                s_pathSrc[0]       = '\0';      // forza rebuild path display in Save tab
                s_pathDst[0]       = '\0';
            }
        }

        // Ctrl+Z: undo ultimo placement
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (!g_placements.empty()) {
                int last = (int)g_placements.size()-1;
                if (s_selectedPlacementIdx == last) {
                    s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1;
                }
                remove_placement(last);
                if (s_selectedPlacementIdx >= (int)g_placements.size()) {
                    int ns = (int)g_placements.size()-1;
                    if (ns >= 0) select_placement(ns);
                    else { s_hijackEntity = nullptr; s_previewActive = false; s_selectedPlacementIdx = -1; s_editingExisting = false; s_selectedEntIdx = -1; }
                }
            }
        }
        draw_sidebar();
        draw_content();
        draw_gizmos();
        draw_inspector_panel();
    }

} // namespace imgui_layer
