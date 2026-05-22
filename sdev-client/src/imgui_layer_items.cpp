#include "include/imgui_layer_internal.h"
#include <external/stb/stb_image.h>
#include "include/shaiya/CDataFile.h"
#include "include/shaiya/CItem.h"
#include "include/shaiya/CTexture.h"
#include "include/shaiya/ItemInfo.h"
#include "resources/resource.h"
#pragma comment(lib, "d3d9.lib")

namespace imgui_layer {

    int count_inventory_item(uint8_t type, uint8_t typeId)
    {
        auto count = 0;
        auto maxBag = g_pPlayerData->inventory.size() - 1;

        for (size_t bag = 1; bag <= maxBag; ++bag)
        {
            for (const auto& item : g_pPlayerData->inventory[bag])
            {
                if (item.type == type && item.typeId == typeId)
                    count += item.count;
            }
        }

        return count;
    }

    int resolve_item_icon_index(int type, int typeId)
    {
        auto* itemInfo = CDataFile::GetItemInfo(type, typeId);
        if (!itemInfo)
            return -1;

        return static_cast<int>(itemInfo->icon);
    }

    const char* get_native_item_icon_folder()
    {
        static constexpr const char* kDefaultIconFolder = "data/interface/icon";
        auto kConfiguredIconFolder = reinterpret_cast<const char*>(0x7AC104);
        if (GetFileAttributesA(kDefaultIconFolder) != INVALID_FILE_ATTRIBUTES)
            return kDefaultIconFolder;
        if (kConfiguredIconFolder && kConfiguredIconFolder[0])
            return kConfiguredIconFolder;
        return kDefaultIconFolder;
    }

    CTexture* get_or_load_native_item_icon_texture(int iconId)
    {
        if (iconId < 0)
            return nullptr;

        auto& entry = g_nativeItemIcons[iconId];
        if (!entry.loadAttempted)
        {
            entry.iconId = iconId;
            entry.loadAttempted = true;

            char fileName[32]{};
            std::snprintf(fileName, sizeof(fileName), iconId < 100 ? "%02d.dds" : "%d.dds", iconId);
            if (!CTexture::CreateFromFile(&entry.texture, get_native_item_icon_folder(), fileName, 32, 32))
            {
                std::snprintf(fileName, sizeof(fileName), iconId < 100 ? "%02d.tga" : "%d.tga", iconId);
                CTexture::CreateFromFile(&entry.texture, get_native_item_icon_folder(), fileName, 32, 32);
            }
        }

        return entry.texture.texture ? &entry.texture : nullptr;
    }

    bool get_item_icon_atlas_config(int type, std::string& outFileName, int& outCols, int& outRows, int& outWidth, int& outHeight)
    {
        auto set_numeric_atlas = [&](int atlasType) -> bool
            {
                char fileName[16]{};
                std::snprintf(fileName, sizeof(fileName), atlasType < 100 ? "%02d.dds" : "%d.dds", atlasType);
                outFileName = fileName;
                outCols = 4;
                outRows = 16;
                outWidth = 128;
                outHeight = 512;
                return true;
            };

        auto set_common_atlas = [&](const char* fileName) -> bool
            {
                outFileName = fileName;
                if (_stricmp(fileName, "icon_rapis.dds") == 0)
                {
                    outCols = 8;
                    outRows = 16;
                    outWidth = 256;
                    outHeight = 512;
                    return true;
                }

                outCols = 16;
                outRows = 16;
                outWidth = 512;
                outHeight = 512;
                return true;
            };

        switch (type)
        {
        case 1:  return set_numeric_atlas(1);
        case 2:  return set_numeric_atlas(2);
        case 3:  return set_numeric_atlas(3);
        case 4:  return set_numeric_atlas(4);
        case 5:  return set_numeric_atlas(5);
        case 6:  return set_numeric_atlas(6);
        case 7:  return set_numeric_atlas(7);
        case 8:  return set_numeric_atlas(8);
        case 9:  return set_numeric_atlas(9);
        case 10: return set_numeric_atlas(10);
        case 11: return set_numeric_atlas(11);
        case 12: return set_numeric_atlas(12);
        case 13: return set_numeric_atlas(13);
        case 14: return set_numeric_atlas(14);
        case 15: return set_numeric_atlas(15);
        case 16: return set_numeric_atlas(16);
        case 17: return set_numeric_atlas(17);
        case 18: return set_numeric_atlas(18);
        case 19: return set_numeric_atlas(19);
        case 20: return set_numeric_atlas(20);
        case 21: return set_numeric_atlas(21);
        case 22: return set_numeric_atlas(22);
        case 23: return set_numeric_atlas(23);
        case 24: return set_numeric_atlas(24);
        case 25: return set_common_atlas("icon_somo.dds");
        case 26: return set_common_atlas("icon_somo.dds");
        case 27: return set_common_atlas("icon_somo.dds");
        case 28: return set_common_atlas("icon_somo.dds");
        case 29: return set_common_atlas("icon_somo.dds");
        case 30: return set_common_atlas("icon_rapis.dds");
        case 31: return set_numeric_atlas(31);
        case 32: return set_numeric_atlas(32);
        case 33: return set_numeric_atlas(33);
        case 34: return set_numeric_atlas(34);
        case 35: return set_numeric_atlas(35);
        case 36: return set_numeric_atlas(36);
        case 37: return set_numeric_atlas(37);
        case 38: return set_numeric_atlas(38);
        case 39: return set_numeric_atlas(39);
        case 40: return set_numeric_atlas(40);
        case 41: return set_common_atlas("icon_somo.dds");
        case 42: return set_common_atlas("icon_somo.dds");
        case 43: return set_common_atlas("icon_somo.dds");
        case 44: return set_common_atlas("icon_somo.dds");
        case 45: return set_numeric_atlas(1);
        case 46: return set_numeric_atlas(2);
        case 47: return set_numeric_atlas(3);
        case 48: return set_numeric_atlas(4);
        case 49: return set_numeric_atlas(5);
        case 50: return set_numeric_atlas(5);
        case 51: return set_numeric_atlas(6);
        case 52: return set_numeric_atlas(6);
        case 53: return set_numeric_atlas(7);
        case 54: return set_numeric_atlas(7);
        case 55: return set_numeric_atlas(8);
        case 56: return set_numeric_atlas(8);
        case 57: return set_numeric_atlas(9);
        case 58: return set_numeric_atlas(10);
        case 59: return set_numeric_atlas(11);
        case 60: return set_numeric_atlas(12);
        case 61: return set_numeric_atlas(12);
        case 62: return set_numeric_atlas(13);
        case 63: return set_numeric_atlas(13);
        case 64: return set_numeric_atlas(14);
        case 65: return set_numeric_atlas(15);
        case 66: return set_numeric_atlas(16);
        case 67: return set_numeric_atlas(17);
        case 68: return set_numeric_atlas(18);
        case 69: return set_numeric_atlas(19);
        case 70: return set_numeric_atlas(20);
        case 71: return set_numeric_atlas(21);
        case 72: return set_numeric_atlas(16);
        case 73: return set_numeric_atlas(17);
        case 74: return set_numeric_atlas(18);
        case 76: return set_numeric_atlas(20);
        case 77: return set_numeric_atlas(21);
        case 82: return set_numeric_atlas(32);
        case 83: return set_numeric_atlas(33);
        case 84: return set_numeric_atlas(34);
        case 85: return set_numeric_atlas(35);
        case 86: return set_numeric_atlas(36);
        case 87: return set_numeric_atlas(31);
        case 88: return set_numeric_atlas(32);
        case 89: return set_numeric_atlas(33);
        case 91: return set_numeric_atlas(34);
        case 92: return set_numeric_atlas(35);
        case 94: return set_common_atlas("icon_somo.dds");
        case 95: return set_common_atlas("icon_rapis.dds");
        case 96: return set_numeric_atlas(23);
        case 97: return set_numeric_atlas(40);
        case 99: return set_common_atlas("icon_somo.dds");
        case 100: return set_common_atlas("icon_somo2.dds");
        case 101: return set_common_atlas("icon_somo2.dds");
        case 102: return set_common_atlas("icon_somo3.dds");
        case 120: return set_common_atlas("icon_pet.dds");
        case 121: return set_common_atlas("icon_Wing.dds");
        case 150: return set_common_atlas("icon_DualLayer.dds");
        default:
            return false;
        }
    }

    int get_item_icon_atlas_resource_id(const std::string& fileName)
    {
        if (fileName.size() == 6 && fileName[2] == '.' && _stricmp(fileName.c_str() + 2, ".dds") == 0)
        {
            auto type = (fileName[0] - '0') * 10 + (fileName[1] - '0');
            if (type >= 1 && type <= 24)
                return IDR_ITEM_ICON_01 + type - 1;
            if (type >= 31 && type <= 40)
                return IDR_ITEM_ICON_31 + type - 31;
        }

        if (_stricmp(fileName.c_str(), "icon_DualLayer.dds") == 0)
            return IDR_ITEM_ICON_DUALLAYER;
        if (_stricmp(fileName.c_str(), "icon_pet.dds") == 0)
            return IDR_ITEM_ICON_PET;
        if (_stricmp(fileName.c_str(), "icon_rapis.dds") == 0)
            return IDR_ITEM_ICON_RAPIS;
        if (_stricmp(fileName.c_str(), "icon_somo.dds") == 0)
            return IDR_ITEM_ICON_SOMO;
        if (_stricmp(fileName.c_str(), "icon_somo2.dds") == 0)
            return IDR_ITEM_ICON_SOMO2;
#ifdef IDR_ITEM_ICON_SOMO3
        if (_stricmp(fileName.c_str(), "icon_somo3.dds") == 0)
            return IDR_ITEM_ICON_SOMO3;
#endif
        if (_stricmp(fileName.c_str(), "icon_Wing.dds") == 0)
            return IDR_ITEM_ICON_WING;

        return 0;
    }

    LPDIRECT3DTEXTURE9 load_item_icon_loose_texture(const std::string& fileName)
    {
        if (!g_device || fileName.empty())
            return nullptr;

        auto* folder = get_native_item_icon_folder();
        if (!folder || !folder[0])
            return nullptr;

        std::string path = folder;
        if (!path.empty() && path.back() != '\\' && path.back() != '/')
            path.push_back('\\');
        path += fileName;

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return nullptr;

        std::vector<char> fileData(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        if (fileData.empty())
            return nullptr;

        return create_texture_from_image_memory(g_device, fileData.data(), static_cast<UINT>(fileData.size()));
    }

    LPDIRECT3DTEXTURE9 load_item_icon_resource_texture(const std::string& fileName)
    {
        if (!g_device)
            return nullptr;

        auto resourceId = get_item_icon_atlas_resource_id(fileName);
        if (!resourceId)
            return nullptr;

        auto module = GetModuleHandleA("sdev-client.dll");
        if (!module)
            module = GetModuleHandleA("sdev.dll");
        if (!module)
            module = GetModuleHandleA(nullptr);

        auto resource = FindResourceA(module, MAKEINTRESOURCEA(resourceId), MAKEINTRESOURCEA(10));
        if (!resource)
            return nullptr;

        auto resourceHandle = LoadResource(module, resource);
        auto resourceData = resourceHandle ? LockResource(resourceHandle) : nullptr;
        auto resourceSize = SizeofResource(module, resource);
        if (!resourceData || !resourceSize)
            return load_item_icon_loose_texture(fileName);

        return create_texture_from_image_memory(g_device, resourceData, resourceSize);
    }

    LPDIRECT3DTEXTURE9 get_or_load_item_icon_atlas_texture(const std::string& fileName, int width, int height)
    {
        auto& entry = g_itemIconAtlases[fileName];
        if (entry.fileName.empty())
        {
            entry.fileName = fileName;
            entry.width = width;
            entry.height = height;
        }

        if (!entry.loadAttempted)
        {
            entry.loadAttempted = true;
            entry.texture = load_item_icon_resource_texture(fileName);
        }

        return entry.texture;
    }

    void draw_item_icon_at(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, int type, int typeId)
    {
        if (!drawList)
            return;

        auto iconIndex = resolve_item_icon_index(type, typeId);
        std::string atlasFileName;
        int atlasCols = 0;
        int atlasRows = 0;
        int atlasWidth = 0;
        int atlasHeight = 0;
        if (get_item_icon_atlas_config(type, atlasFileName, atlasCols, atlasRows, atlasWidth, atlasHeight))
        {
            if (auto atlasTexture = get_or_load_item_icon_atlas_texture(atlasFileName, atlasWidth, atlasHeight))
            {
                if (iconIndex > 0)
                {
                    auto atlasIndex = iconIndex - 1;
                    if (atlasIndex >= 0 && atlasIndex < atlasCols * atlasRows)
                    {
                        auto col = static_cast<float>(atlasIndex % atlasCols);
                        auto row = static_cast<float>(atlasIndex / atlasCols);
                        ImVec2 uvMin(col / static_cast<float>(atlasCols), row / static_cast<float>(atlasRows));
                        ImVec2 uvMax((col + 1.0f) / static_cast<float>(atlasCols), (row + 1.0f) / static_cast<float>(atlasRows));
                        drawList->AddImage(reinterpret_cast<ImTextureID>(atlasTexture), min, max, uvMin, uvMax);
                        return;
                    }
                }
            }
        }

        drawList->AddRectFilled(min, max, IM_COL32(72, 54, 34, 230), 3.0f);
        drawList->AddRect(min, max, IM_COL32(170, 135, 78, 210), 3.0f);
    }

    void draw_item_count_badge(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, int count)
    {
        if (!drawList || count <= 1)
            return;

        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%d", count);
        auto textSize = ImGui::CalcTextSize(buffer);
        auto textPos = ImVec2(max.x - textSize.x - 3.0f, max.y - textSize.y - 1.0f);
        drawList->AddText(ImVec2(textPos.x - 1.0f, textPos.y), IM_COL32(0, 0, 0, 210), buffer);
        drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y), IM_COL32(0, 0, 0, 210), buffer);
        drawList->AddText(ImVec2(textPos.x, textPos.y - 1.0f), IM_COL32(0, 0, 0, 210), buffer);
        drawList->AddText(ImVec2(textPos.x, textPos.y + 1.0f), IM_COL32(0, 0, 0, 210), buffer);
        drawList->AddText(textPos, IM_COL32(255, 244, 210, 255), buffer);
    }

    // DDS header structures (subset needed for DXT-compressed icon atlases)
#pragma pack(push, 1)
    struct DDS_PIXELFORMAT
    {
        DWORD dwSize;
        DWORD dwFlags;
        DWORD dwFourCC;
        DWORD dwRGBBitCount;
        DWORD dwRBitMask;
        DWORD dwGBitMask;
        DWORD dwBBitMask;
        DWORD dwABitMask;
    };

    struct DDS_HEADER
    {
        DWORD dwMagic;       // 'DDS '
        DWORD dwSize;        // 124
        DWORD dwFlags;
        DWORD dwHeight;
        DWORD dwWidth;
        DWORD dwPitchOrLinearSize;
        DWORD dwDepth;
        DWORD dwMipMapCount;
        DWORD dwReserved1[11];
        DDS_PIXELFORMAT ddspf;
        DWORD dwCaps;
        DWORD dwCaps2;
        DWORD dwCaps3;
        DWORD dwCaps4;
        DWORD dwReserved2;
    };
#pragma pack(pop)

    constexpr DWORD kDdsMagic = 0x20534444; // 'DDS '
    constexpr DWORD kDdpfFourCC = 0x4;

    static DWORD make_fourcc(char a, char b, char c, char d)
    {
        return static_cast<DWORD>(a) | (static_cast<DWORD>(b) << 8)
            | (static_cast<DWORD>(c) << 16) | (static_cast<DWORD>(d) << 24);
    }

    LPDIRECT3DTEXTURE9 create_texture_from_dds_memory(LPDIRECT3DDEVICE9 device, const void* data, UINT dataSize)
    {
        if (!device || !data || dataSize < sizeof(DDS_HEADER))
            return nullptr;

        auto* header = static_cast<const DDS_HEADER*>(data);
        if (header->dwMagic != kDdsMagic)
            return nullptr;

        auto width = header->dwWidth;
        auto height = header->dwHeight;
        if (width == 0 || height == 0)
            return nullptr;

        D3DFORMAT format = D3DFMT_UNKNOWN;
        UINT blockSize = 16;
        if (header->ddspf.dwFlags & kDdpfFourCC)
        {
            auto fourCC = header->ddspf.dwFourCC;
            if (fourCC == make_fourcc('D', 'X', 'T', '1')) { format = D3DFMT_DXT1; blockSize = 8; }
            else if (fourCC == make_fourcc('D', 'X', 'T', '3')) { format = D3DFMT_DXT3; }
            else if (fourCC == make_fourcc('D', 'X', 'T', '5')) { format = D3DFMT_DXT5; }
        }

        if (format == D3DFMT_UNKNOWN)
            return nullptr;

        auto pixelDataOffset = static_cast<UINT>(sizeof(DWORD) + header->dwSize);
        if (pixelDataOffset >= dataSize)
            return nullptr;

        auto pixelDataSize = dataSize - pixelDataOffset;
        auto* pixelData = static_cast<const BYTE*>(data) + pixelDataOffset;

        // Compute expected size for mip level 0
        auto blocksWide = (width + 3) / 4;
        auto blocksHigh = (height + 3) / 4;
        auto expectedSize = blocksWide * blocksHigh * blockSize;
        if (pixelDataSize < expectedSize)
            return nullptr;

        LPDIRECT3DTEXTURE9 texture = nullptr;
        if (FAILED(device->CreateTexture(width, height, 1, 0, format, D3DPOOL_MANAGED, &texture, nullptr)) || !texture)
            return nullptr;

        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            return nullptr;
        }

        // For block-compressed formats, copy row-of-blocks at a time
        auto srcPitch = blocksWide * blockSize;
        for (UINT row = 0; row < blocksHigh; ++row)
        {
            auto* dst = static_cast<BYTE*>(locked.pBits) + row * locked.Pitch;
            auto* src = pixelData + row * srcPitch;
            std::memcpy(dst, src, srcPitch);
        }

        texture->UnlockRect(0);
        return texture;
    }

    LPDIRECT3DTEXTURE9 create_texture_from_png_memory(LPDIRECT3DDEVICE9 device, const void* data, UINT dataSize)
    {
        if (!device || !data || dataSize == 0)
            return nullptr;

        int width = 0, height = 0, channels = 0;
        auto* pixels = stbi_load_from_memory(
            static_cast<const stbi_uc*>(data),
            static_cast<int>(dataSize),
            &width, &height, &channels, 4); // force RGBA
        if (!pixels)
            return nullptr;

        LPDIRECT3DTEXTURE9 texture = nullptr;
        if (FAILED(device->CreateTexture(
            static_cast<UINT>(width), static_cast<UINT>(height),
            1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) || !texture)
        {
            stbi_image_free(pixels);
            return nullptr;
        }

        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            stbi_image_free(pixels);
            return nullptr;
        }

        // stb_image outputs RGBA, D3D9 expects BGRA (A8R8G8B8) — swizzle R<->B
        for (int y = 0; y < height; ++y)
        {
            auto* src = pixels + y * width * 4;
            auto* dst = static_cast<BYTE*>(locked.pBits) + y * locked.Pitch;
            for (int x = 0; x < width; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 2]; // B
                dst[x * 4 + 1] = src[x * 4 + 1]; // G
                dst[x * 4 + 2] = src[x * 4 + 0]; // R
                dst[x * 4 + 3] = src[x * 4 + 3]; // A
            }
        }

        texture->UnlockRect(0);
        stbi_image_free(pixels);
        return texture;
    }

    // Auto-detect format by header and load accordingly (replaces D3DXCreateTextureFromFileInMemory)
    LPDIRECT3DTEXTURE9 create_texture_from_image_memory(LPDIRECT3DDEVICE9 device, const void* data, UINT dataSize)
    {
        if (!device || !data || dataSize < 4)
            return nullptr;

        auto magic = *static_cast<const DWORD*>(data);

        // DDS: 'DDS ' magic
        if (magic == kDdsMagic)
            return create_texture_from_dds_memory(device, data, dataSize);

        // PNG / JPEG / BMP / TGA — anything stb_image supports
        return create_texture_from_png_memory(device, data, dataSize);
    }

    void release_item_icon_textures()
    {
        for (auto& [_, entry] : g_itemIconAtlases)
        {
            if (entry.texture)
            {
                entry.texture->Release();
                entry.texture = nullptr;
            }
            entry.loadAttempted = false;
        }
    }

} // namespace imgui_layer
