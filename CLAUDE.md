# Istruzioni — Shaiya-Core

DLL client iniettata in `game.exe` (x86 32-bit, MSVC). Aggiunge hook, overlay ImGui,
pannelli debug e funzionalità custom al client Shaiya EP6.

---

## MCP GhidraAssist — Regola Fondamentale

**Istanza unica `127.0.0.1:8080` gestisce tutti i binari. Richiedere all'utente di aprire Ghidra con i binari necessari e lasciarli in background.**

```
mcp__ghidrassistmcp__list_binaries              → vedi tutti i binari aperti
mcp__ghidrassistmcp__get_binary_info program_name="game.exe"
```

### Binari in questo progetto

| `program_name` | Lato | Progetto | Note |
|---|---|---|---|
| `game.exe` | **CLIENT** | `sdev-client` | Hook, ImGui overlay — questo progetto |
| `ps_game.exe` | **SERVER** | `sdev.dll` | Server di gioco principale |
| `ps_login.exe` | **SERVER** | `sdev-login.dll` | Login / autenticazione |
| `ps_dbAgent.exe` | **SERVER** | `sdev-db.dll` | Database agent |
| `ps_session.exe` | **SERVER** | — | Gestione sessioni |
| `ps_gameLog.exe` | **SERVER** | — | Logging partite |
| `ps_userLog.exe` | **SERVER** | — | Logging utenti |

**Usare sempre `program_name` esplicito** — non dipendere dalla finestra attiva in Ghidra.

Lavoro su `sdev-client` (hooks, overlay) → `program_name="game.exe"`
Lavoro su comunicazione server/client → `program_name="ps_game.exe"` o altro ps_*

### Workflow per trovare un nuovo hook

1. Identificare l'indirizzo di partenza (da simbolo noto, cross-ref, o pattern bytes)
2. Disassemblare almeno 128 bytes per vedere il contesto completo:
   ```
   mcp__ghidrassistmcp__disassemble_at  address=0xXXXXXX  length=128
   ```
3. Verificare i bytes esatti delle istruzioni da spostare (displaced)
4. Calcolare la size: somma bytes delle istruzioni fino a coprire almeno 5 bytes
5. Identificare l'indirizzo di continuazione (primo byte dopo le istruzioni displaced)
6. Scrivere la naked function con i bytes displaced corretti
7. Installare con `util::detour((void*)0xXXXXXX, naked_fn, size)`

### Warning critico — AI vs MCP

L'AI senza MCP può produrre disassembly completamente sbagliato.
Esempio reale: il hook a `0x44EBF1` fu progettato su bytes inventati dall'AI
(`mov ecx, [esp+0x10]`) mentre il binario reale aveva `XOR BL,BL` (2 bytes).
Risultato: crash immediato del gioco.

**Non scrivere mai un hook senza aver verificato i bytes reali con `disassemble_at`.**

---

## Struttura del progetto

```
sdev-client/src/          — sorgenti della DLL client
  hook.cpp                — punto di installazione di tutti gli hook
  ui_image.cpp            — hook per PVP rank icons + rank override
  title.cpp               — hook rendering title/nameplate
  name_color.cpp          — hook colore nome personaggio
  packet.cpp              — hook pacchetti di rete (es. 0x229 revenge mark)
  imgui_layer_*.cpp       — pannelli ImGui (pannello principale, debug, roulette, ecc.)
  imgui_layer_character_visual_debug.cpp  — "Character Visual Debug" panel

sdev-client/src/include/
  imgui_layer_internal.h  — header condiviso tra tutti i imgui_layer_*.cpp
  shaiya/CCharacter.h     — struttura CCharacter (personaggio)
  shaiya/Static.h         — struttura Static (g_var, globals del gioco)
  shaiya/CWorldMgr.h      — CWorldMgr (world manager, lista personaggi)

util/util.cpp             — implementazione di util::detour, read/write_memory
```

---

## Pattern hook standard

```cpp
// FILE: sdev-client/src/<modulo>.cpp
#include <util/util.h>
#include "include/main.h"

unsigned u0xCONTINUATION = 0x00CONTINUATION;

void __declspec(naked) naked_0xHOOK_ADDR()
{
    __asm
    {
        // --- bytes displaced (copiare esattamente dal disassembly MCP) ---
        istruzione_1
        istruzione_2    // fino a >= 5 bytes totali

        // --- logica custom (opzionale: chiamare funzione C++) ---
        push    arg
        call    my_cpp_function
        add     esp, N   // cdecl cleanup

        jmp     u0xCONTINUATION
    }
}

void hook::nome_modulo()
{
    util::detour((void*)0x00HOOK_ADDR, naked_0xHOOK_ADDR, SIZE);
}
```

### Regole naked function

- Usare `push`/`pop` o `pushad`/`popad` per preservare i registri se la logica custom li modifica
- EBX, ESI, EDI, EBP sono callee-saved in MSVC cdecl — le funzioni C++ chiamate li preservano
- EAX, ECX, EDX possono essere modificati da funzioni C++ chiamate — salvarli se servono dopo
- FLAGS: `cmp` modifica i flags — non importa se l'istruzione successiva nel gioco li ridefinisce
- Verificare sempre se il codice dopo il hook dipende da un registro specifico

### Chiamata a funzione C++ da naked ASM (pattern da name_color.cpp)

```cpp
// Funzione C++ normale — può accedere a qualsiasi variabile/namespace
int32_t my_helper(int32_t param)
{
    if (!imgui_layer::g_someFlag)
        return param;
    return imgui_layer::g_someOverride;
}

// Nella naked function:
__asm
{
    push    ecx          // argomento
    call    my_helper
    add     esp, 4       // cdecl cleanup
    mov     ecx, eax     // usa il risultato
}
```

---

## Indirizzi hook noti (game.exe)

| Indirizzo | Funzione | Size | Descrizione |
|---|---|---|---|
| `0x44EC5C` | `FUN_0044ebe0` | 8 | Rank icon X position (+3) |
| `0x44ECBC` | `FUN_0044ebe0` | 7 | Rank icon Y position (+5) |
| `0x44EBED` | `FUN_0044ebe0` | 6 | killLv override per rank icon |
| `0x453CDB` | name draw | 6 | Colore nome personaggio (helmet) |
| `0x453E7C` | title draw | 5 | Hook title/nameplate rendering |
| `0x4EF2F3` | packet 0x229 | 5 | Revenge mark / kill count msg |

### Struttura `FUN_0044ebe0` (rank icon selection loop)

```asm
0044ebe0: SUB ESP, 0x8
0044ebe3: FLDZ
0044ebe5: PUSH EBX
0044ebe6: PUSH ESI
0044ebe7: FSTP [ESP+0x8]
0044ebeb: MOV ESI, ECX          ; this pointer
0044ebed: MOV ECX, [ESP+0x20]  ; killLv parameter  ← HOOK QUI (size=6)
0044ebf1: XOR BL, BL            ; init rank result = 0
0044ebf3: MOV EAX, 0x1E         ; start from rank 30
0044ebf8: JMP 0x44EC00
0044ec00: CMP ECX, [EAX*4 + 0x7aee08]  ; confronta killLv con tabella soglie
0044ec07: JGE 0x44EC10
0044ec09: SUB EAX, 0x1
0044ec0c: JNS 0x44EC00
; ... selezione texture e draw ...
0044ec5c: MOV EAX, [ESP+0x28]  ; X position  ← hook esistente (+3)
0044ecbc: (Y position)          ← hook esistente (+5)
```

- Tabella soglie rank: `0x7AEE08` (31 entries, indici 0x00–0x1E)
- `[ESP+0x20]` all'interno di `FUN_0044ebe0` = parametro killLv (stack offset dopo il prologue da 16 bytes)

---

## Globals utili (da Static.h / CCharacter.h)

| Simbolo | Indirizzo | Tipo | Descrizione |
|---|---|---|---|
| `g_var->killLv` | `0x22B1AF8` | `int32_t` | Kill rank level (global Static) |
| `g_var->deathLv` | `0x22B1AFC` | `int32_t` | Death rank level |
| `g_pWorldMgr->user` | — | `CCharacter*` | Personaggio locale |
| `CCharacter::kills` | offset `0x334` | `uint32_t` | Kill count del personaggio |

---

## Variabili ImGui debug (imgui_layer_internal.h)

Tutte le variabili condivise tra i pannelli sono `inline` nel namespace `imgui_layer`.
I file `imgui_layer_*.cpp` aprono `namespace imgui_layer { ... }` quindi le usano senza prefisso.

Per accedere da naked ASM: usare un helper C++ come intermediario (non referenziare
variabili namespace da inline ASM direttamente).

---

## Mappe Shaiya — Formato e Alpha DDS

**Riferimento**: https://github.com/matigramirez/Parsec (Parsec + Unity World Editor by Mati)

### File che compongono una mappa

| File | Posizione client | Note |
|---|---|---|
| `{mapId}.wld` | `Data\World\` | Terrain heights, texture layers, entities, portals… |
| Alpha DDS | `Data\World\Field\{mapId}\` | Peso blend per ogni layer terreno (una per patch×layer) |
| Light DDS | `Data\World\Field\{mapId}\` | Lightmap per ogni patch |
| `.svmap` | — | Dati navigazione/spawn (server) |
| `.zon` | — | Zone definitions |

### Alpha DDS — formato verificato

- **Nome file**: `{mapId}_{row}{col}_A{layerIndex}.dds` → es. `2_00_A0.dds`
- **Formato**: **DXT3** (BC2) — CONFERMATO dai file originali mappa 2
- **Dimensioni**: 256×256 pixel, 9 mip levels
- **Payload totale**: 87408 byte (header DDS = 128 byte → file totale = 87536 byte)
- **Struttura DXT3 block** (16 byte per blocco 4×4):
  - Byte 0-7: alpha a 4-bit per pixel (16 nibbles packed, usato per il blend weight)
  - Byte 8-15: color block DXT1 (RGB565)
- Percorso corretto: `Data\World\Field\{mapId}\` (**NON** `Data\Field\{mapId}\`)

### Alpha DDS — semantica dei layer

- Ogni patch ha una DDS per ogni texture layer (A0, A1, A2…)
- Il valore alpha (0-15 in DXT3, 0=trasparente, 15=opaco) = quanto quel layer è visibile nella cella
- Max layer originali Shaiya: **8**
- Brush size consigliato (Unity/tool): **≥ 8** — valori più piccoli si perdono per via della bassa risoluzione delle alpha

### Patch layout (mappa 2)

- `M = *(int*)(0x7C4A74 + 4)` = patch per lato = **2** per mappa 2
- `N = 1025` → `cellsPerPatch = (N-1)/M = 512`
- Patch naming: `{row}{col}` con row/col da 0 a M-1 → `00`, `01`, `10`, `11`

### Mip chain DXT3 256×256 (9 mip)

```
Mip 0: 64×64 blocks × 16B = 65536 B
Mip 1: 32×32 × 16B        = 16384 B
Mip 2: 16×16 × 16B        =  4096 B
Mip 3:  8×8  × 16B        =  1024 B
Mip 4:  4×4  × 16B        =   256 B
Mip 5:  2×2  × 16B        =    64 B
Mip 6:  1×1  × 16B        =    16 B
Mip 7:  1×1  × 16B        =    16 B  (min 1 block)
Mip 8:  1×1  × 16B        =    16 B
Totale                     = 87408 B
```

### Errori noti / bug corretti

- Bug precedente: leggeva 65536 byte come pixel L8 lineari → scriveva DDS corrotto 65KB → terreno bianco
- Fix: legge 87408 byte DXT3 completi, modifica alpha nibbles nei blocchi corretti, riscrive file intero
- `srcDir` corretto: `{dllDir}\Data\World\Field\{mapId}\` (il DLL è nella root del client)

---

## Aggiungere un nuovo pannello debug

1. Aggiungere la funzione `draw_*_section()` in `imgui_layer_character_visual_debug.cpp`
2. Chiamarla in `draw_character_layout_debug_panel()` nell'ordine desiderato
3. Se serve overlay 2D: usare `draw_local_character_guides()` con `g_characterLayoutAnchorScreen`
4. Se serve uno slot immagine: aggiungere a `CharacterDebugImageSlot` enum in `imgui_layer_internal.h`
   e aggiornare `g_characterDebugImageOverrides` array (si ridimensiona automaticamente)
5. Aggiornare `build_character_layout_dump()` per includere i nuovi valori
