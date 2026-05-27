#include "include/imgui_layer_internal.h"
#include "include/game_data_archive.h"
#include <filesystem>
#include <set>

namespace imgui_layer
{
    namespace
    {
        constexpr std::array<const char*, 7> kSupportedImageExt{
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".gif"
        };

        struct ArchiveAssetImageEntry
        {
            std::string candidatePath;
            std::string normalizedCandidatePath;
            std::string fileName;
            std::string normalizedFileName;
            std::uint64_t offset = 0;
            std::uint64_t size = 0;
        };

        std::vector<std::string> g_assetImageCandidates;
        bool g_assetImageCandidatesLoaded = false;
        std::vector<std::string> g_assetRootDebugPaths;
        std::vector<ArchiveAssetImageEntry> g_archiveAssetImages;
        bool g_archiveAssetImagesLoaded = false;

        bool is_absolute_windows_path(const std::string& path)
        {
            return path.size() > 2
                && std::isalpha(static_cast<unsigned char>(path[0]))
                && path[1] == ':';
        }

        bool file_exists(const std::string& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return static_cast<bool>(stream);
        }

        bool directory_exists(const std::string& path)
        {
            auto attrs = GetFileAttributesA(path.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        std::string to_lower_slash_path(std::string value)
        {
            std::replace(value.begin(), value.end(), '/', '\\');
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string trim_path_input(std::string value)
        {
            auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());
            while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
                value.pop_back();

            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);

            return value;
        }

        bool is_supported_image_extension(const std::string& extension)
        {
            auto normalized = to_lower_slash_path(extension);
            return std::find_if(kSupportedImageExt.begin(), kSupportedImageExt.end(), [&](const char* ext) {
                return normalized == ext;
            }) != kSupportedImageExt.end();
        }

        void ensure_archive_asset_index(bool forceRefresh = false)
        {
            if (g_archiveAssetImagesLoaded && !forceRefresh)
                return;

            g_archiveAssetImages.clear();
            std::set<std::string> unique;
            game_data::scan_sah_files([&](const game_data::SahFileEntry& entry) {
                if (!is_supported_image_extension(std::filesystem::path(entry.fileName).extension().string()))
                    return;

                auto normalizedPath = game_data::lower_ascii(entry.path);
                if (!(normalizedPath == "assets"
                    || normalizedPath.rfind("assets\\", 0) == 0
                    || normalizedPath == "data\\assets"
                    || normalizedPath.rfind("data\\assets\\", 0) == 0))
                    return;

                auto candidatePath = entry.path.empty() ? entry.fileName : entry.path + "\\" + entry.fileName;
                std::replace(candidatePath.begin(), candidatePath.end(), '/', '\\');
                auto normalizedCandidatePath = to_lower_slash_path(candidatePath);
                if (normalizedCandidatePath.rfind("data\\", 0) == 0)
                    normalizedCandidatePath = normalizedCandidatePath.substr(5);
                if (normalizedCandidatePath.rfind("assets\\", 0) != 0 && normalizedCandidatePath != "assets")
                    return;
                if (!unique.insert(normalizedCandidatePath).second)
                    return;

                auto fileName = std::filesystem::path(candidatePath).filename().string();
                g_archiveAssetImages.push_back(ArchiveAssetImageEntry{
                    normalizedCandidatePath,
                    normalizedCandidatePath,
                    fileName,
                    to_lower_slash_path(fileName),
                    entry.offset,
                    entry.size
                });
            });

            std::sort(g_archiveAssetImages.begin(), g_archiveAssetImages.end(), [](const ArchiveAssetImageEntry& a, const ArchiveAssetImageEntry& b) {
                return a.normalizedCandidatePath < b.normalizedCandidatePath;
            });
            g_archiveAssetImagesLoaded = true;
        }

        std::vector<std::string> build_asset_roots()
        {
            namespace fs = std::filesystem;

            std::set<std::string> uniqueRoots;
            std::set<std::string> uniqueSeedDirs;
            std::vector<std::string> roots;
            g_assetRootDebugPaths.clear();

            auto add_root = [&](const fs::path& path) {
                auto pathString = path.string();
                if (pathString.empty() || !directory_exists(pathString))
                    return;

                auto normalized = to_lower_slash_path(pathString);
                if (!uniqueRoots.insert(normalized).second)
                    return;

                roots.push_back(pathString);
                g_assetRootDebugPaths.push_back(pathString);
            };

            auto add_seed_dir = [&](const fs::path& path) {
                if (path.empty())
                    return;

                auto current = path;
                for (int depth = 0; depth < 10 && !current.empty(); ++depth)
                {
                    auto currentString = current.string();
                    if (!currentString.empty() && directory_exists(currentString))
                    {
                        auto normalized = to_lower_slash_path(currentString);
                        if (uniqueSeedDirs.insert(normalized).second)
                        {
                            add_root(current / "Data" / "Assets");
                            add_root(current / "data" / "assets");
                            add_root(current / "Data" / "assets");
                            add_root(current / "data" / "Assets");
                            add_root(current / "Assets");
                            add_root(current / "assets");
                        }
                    }

                    auto parent = current.parent_path();
                    if (parent.empty() || parent == current)
                        break;
                    current = parent;
                }
            };

            char moduleFileName[MAX_PATH]{};
            if (GetModuleFileNameA(nullptr, moduleFileName, MAX_PATH))
                add_seed_dir(fs::path(moduleFileName).parent_path());

            char currentDirectory[MAX_PATH]{};
            if (GetCurrentDirectoryA(MAX_PATH, currentDirectory) > 0)
                add_seed_dir(fs::path(currentDirectory));

            add_seed_dir(fs::path(get_game_base_dir()));

            if (g_var && g_var->iniFileName.data() && g_var->iniFileName.data()[0])
                add_seed_dir(fs::path(g_var->iniFileName.data()).parent_path());

            return roots;
        }

        template <typename TVisitor>
        void scan_directory_recursive(const std::string& root, TVisitor&& visitor)
        {
            std::vector<std::string> pending{ root };
            while (!pending.empty())
            {
                auto current = pending.back();
                pending.pop_back();

                std::string pattern = current;
                if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
                    pattern += '\\';
                pattern += '*';

                WIN32_FIND_DATAA findData{};
                auto handle = FindFirstFileA(pattern.c_str(), &findData);
                if (handle == INVALID_HANDLE_VALUE)
                    continue;

                do
                {
                    auto name = std::string(findData.cFileName);
                    if (name == "." || name == "..")
                        continue;

                    std::string fullPath = current;
                    if (!fullPath.empty() && fullPath.back() != '\\' && fullPath.back() != '/')
                        fullPath += '\\';
                    fullPath += name;

                    if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        pending.push_back(fullPath);
                        continue;
                    }

                    visitor(fullPath);
                }
                while (FindNextFileA(handle, &findData));

                FindClose(handle);
            }
        }

        std::string find_image_in_asset_roots(const std::string& inputPath)
        {
            namespace fs = std::filesystem;

            auto normalizedInput = to_lower_slash_path(inputPath);
            if (normalizedInput.rfind("data\\", 0) == 0)
                normalizedInput = normalizedInput.substr(5);
            auto fileNameOnly = to_lower_slash_path(fs::path(inputPath).filename().string());
            auto hasPathHint = normalizedInput.find('\\') != std::string::npos;

            ensure_archive_asset_index();
            for (const auto& entry : g_archiveAssetImages)
            {
                if (hasPathHint)
                {
                    if (entry.normalizedCandidatePath.size() >= normalizedInput.size()
                        && entry.normalizedCandidatePath.compare(entry.normalizedCandidatePath.size() - normalizedInput.size(), normalizedInput.size(), normalizedInput) == 0)
                        return entry.candidatePath;
                }
                else if (entry.normalizedFileName == fileNameOnly)
                {
                    return entry.candidatePath;
                }
            }

            for (const auto& assetsRoot : build_asset_roots())
            {
                std::string match;
                scan_directory_recursive(assetsRoot, [&](const std::string& candidatePath) {
                    if (!match.empty())
                        return;
                    if (!is_supported_image_extension(fs::path(candidatePath).extension().string()))
                        return;

                    auto candidateNorm = to_lower_slash_path(candidatePath);
                    auto candidateName = to_lower_slash_path(fs::path(candidatePath).filename().string());

                    if (hasPathHint)
                    {
                        if (candidateNorm.size() >= normalizedInput.size()
                            && candidateNorm.compare(candidateNorm.size() - normalizedInput.size(), normalizedInput.size(), normalizedInput) == 0)
                            match = candidatePath;
                    }
                    else if (candidateName == fileNameOnly)
                    {
                        match = candidatePath;
                    }
                });

                if (!match.empty())
                    return match;
            }

            return {};
        }

        int asset_image_match_score(const std::string& candidate, const std::string& normalizedQuery)
        {
            if (normalizedQuery.empty())
                return 0;

            namespace fs = std::filesystem;
            auto normalizedCandidate = to_lower_slash_path(candidate);
            auto fileName = to_lower_slash_path(fs::path(candidate).filename().string());

            if (fileName.rfind(normalizedQuery, 0) == 0)
                return 0;
            if (fileName.find(normalizedQuery) != std::string::npos)
                return 1;
            if (normalizedCandidate.rfind(normalizedQuery, 0) == 0)
                return 2;
            if (normalizedCandidate.find(normalizedQuery) != std::string::npos)
                return 3;
            return 4;
        }
    }

    const std::string& get_game_base_dir()
    {
        return game_data::base_dir();
    }

    std::string get_game_relative_path(const char* relative)
    {
        return game_data::relative_path(relative);
    }

    std::string normalize_asset_browser_text(std::string value)
    {
        return to_lower_slash_path(trim_path_input(std::move(value)));
    }

    std::vector<std::string> collect_asset_image_candidates(bool forceRefresh)
    {
        namespace fs = std::filesystem;

        if (g_assetImageCandidatesLoaded && !forceRefresh)
            return g_assetImageCandidates;

        ensure_archive_asset_index(forceRefresh);

        std::set<std::string> unique;
        std::vector<std::string> candidates;

        for (const auto& entry : g_archiveAssetImages)
        {
            if (unique.insert(entry.normalizedCandidatePath).second)
                candidates.push_back(entry.candidatePath);
        }

        for (const auto& assetsRoot : build_asset_roots())
        {
            scan_directory_recursive(assetsRoot, [&](const std::string& candidatePath) {
                if (!is_supported_image_extension(fs::path(candidatePath).extension().string()))
                    return;

                std::error_code relEc;
                auto relative = fs::relative(fs::path(candidatePath), fs::path(assetsRoot), relEc);
                if (relEc)
                    return;

                auto candidate = std::string("assets\\") + relative.string();
                std::replace(candidate.begin(), candidate.end(), '/', '\\');
                auto normalized = to_lower_slash_path(candidate);
                if (!unique.insert(normalized).second)
                    return;

                candidates.push_back(candidate);
            });
        }

        std::sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b) {
            return to_lower_slash_path(a) < to_lower_slash_path(b);
        });

        g_assetImageCandidates = std::move(candidates);
        g_assetImageCandidatesLoaded = true;
        return g_assetImageCandidates;
    }

    bool asset_image_candidate_matches_filter(const std::string& candidate, const std::string& query)
    {
        auto normalizedQuery = normalize_asset_browser_text(query);
        if (normalizedQuery.empty())
            return true;

        namespace fs = std::filesystem;
        auto normalizedCandidate = to_lower_slash_path(candidate);
        auto fileName = to_lower_slash_path(fs::path(candidate).filename().string());

        return normalizedCandidate.find(normalizedQuery) != std::string::npos
            || fileName.find(normalizedQuery) != std::string::npos;
    }

    bool draw_asset_image_browser(const char* browserId, char* pathBuffer, std::size_t pathBufferSize, char* filterBuffer, std::size_t filterBufferSize, float height)
    {
        auto candidates = collect_asset_image_candidates(false);
        auto normalizedQuery = normalize_asset_browser_text(filterBuffer ? std::string(filterBuffer) : std::string{});
        auto normalizedCurrentPath = normalize_asset_browser_text(pathBuffer ? std::string(pathBuffer) : std::string{});

        if (filterBuffer && filterBufferSize > 0)
        {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint((std::string("##filter_") + browserId).c_str(), "Type to filter assets, e.g. rou", filterBuffer, filterBufferSize);
        }

        if (ImGui::Button((std::string("Refresh List##") + browserId).c_str()))
            candidates = collect_asset_image_candidates(true);

        std::vector<const std::string*> matches;
        matches.reserve(candidates.size());
        for (const auto& candidate : candidates)
        {
            if (asset_image_candidate_matches_filter(candidate, normalizedQuery))
                matches.push_back(&candidate);
        }

        std::stable_sort(matches.begin(), matches.end(), [&](const std::string* a, const std::string* b) {
            auto scoreA = asset_image_match_score(*a, normalizedQuery);
            auto scoreB = asset_image_match_score(*b, normalizedQuery);
            if (scoreA != scoreB)
                return scoreA < scoreB;
            return to_lower_slash_path(*a) < to_lower_slash_path(*b);
        });

        ImGui::TextDisabled("Matches: %d / %d", static_cast<int>(matches.size()), static_cast<int>(candidates.size()));
        if (candidates.empty())
        {
            ImGui::TextDisabled("Archive source: data.sah/saf + filesystem `Data\\Assets` fallback.");
            if (!g_assetRootDebugPaths.empty())
            {
                ImGui::TextDisabled("Filesystem roots:");
                for (const auto& root : g_assetRootDebugPaths)
                    ImGui::TextWrapped("- %s", root.c_str());
            }
        }

        auto childId = std::string("##asset_browser_child_") + browserId;
        ImGui::BeginChild(childId.c_str(), ImVec2(0.0f, height), true, ImGuiWindowFlags_HorizontalScrollbar);

        if (candidates.empty())
        {
            ImGui::TextDisabled("No image files found under archive or filesystem Assets sources.");
            ImGui::EndChild();
            return false;
        }

        if (matches.empty())
        {
            ImGui::TextDisabled("No assets match the current filter.");
            ImGui::EndChild();
            return false;
        }

        for (const auto* candidate : matches)
        {
            auto isSelected = normalize_asset_browser_text(*candidate) == normalizedCurrentPath;
            if (ImGui::Selectable(candidate->c_str(), isSelected))
            {
                if (pathBuffer && pathBufferSize > 0)
                    std::snprintf(pathBuffer, pathBufferSize, "%s", candidate->c_str());
                ImGui::EndChild();
                return true;
            }
        }

        ImGui::EndChild();
        return false;
    }

    std::string resolve_asset_image_path(const std::string& inputPath)
    {
        auto sanitizedInput = trim_path_input(inputPath);
        if (sanitizedInput.empty())
            return {};

        if (is_absolute_windows_path(sanitizedInput))
            return sanitizedInput;

        auto directRelativePath = get_game_relative_path(sanitizedInput.c_str());
        if (file_exists(directRelativePath))
            return directRelativePath;

        auto recursiveMatch = find_image_in_asset_roots(sanitizedInput);
        if (!recursiveMatch.empty())
            return recursiveMatch;

        auto normalizedInput = to_lower_slash_path(sanitizedInput);
        if (normalizedInput.rfind("assets\\", 0) == 0)
        {
            auto relativeInsideAssets = sanitizedInput.substr(7);
            for (const auto& assetsRoot : build_asset_roots())
            {
                std::filesystem::path candidatePath = std::filesystem::path(assetsRoot) / relativeInsideAssets;
                auto candidate = candidatePath.string();
                if (file_exists(candidate))
                    return candidate;
            }
        }

        return directRelativePath;
    }

    bool read_asset_image_bytes(const std::string& inputPath, std::vector<char>& outData, std::string* resolvedPath)
    {
        outData.clear();

        auto sanitizedInput = trim_path_input(inputPath);
        if (sanitizedInput.empty())
            return false;

        auto normalizedInput = to_lower_slash_path(sanitizedInput);
        if (normalizedInput.rfind("data\\", 0) == 0)
            normalizedInput = normalizedInput.substr(5);

        ensure_archive_asset_index();
        for (const auto& entry : g_archiveAssetImages)
        {
            auto pathMatches = entry.normalizedCandidatePath == normalizedInput;
            auto suffixMatches = normalizedInput.find('\\') != std::string::npos
                && entry.normalizedCandidatePath.size() >= normalizedInput.size()
                && entry.normalizedCandidatePath.compare(entry.normalizedCandidatePath.size() - normalizedInput.size(), normalizedInput.size(), normalizedInput) == 0;
            auto fileMatches = entry.normalizedFileName == normalizedInput;
            if (!pathMatches && !suffixMatches && !fileMatches)
                continue;

            if (game_data::read_saf_file(entry.offset, entry.size, outData) && !outData.empty())
            {
                if (resolvedPath)
                    *resolvedPath = entry.candidatePath + " [data.saf]";
                return true;
            }
        }

        auto resolvedFilePath = resolve_asset_image_path(sanitizedInput);
        std::ifstream stream(resolvedFilePath, std::ios::binary);
        if (!stream)
            return false;

        outData.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (outData.empty())
            return false;

        if (resolvedPath)
            *resolvedPath = resolvedFilePath;
        return true;
    }

    bool load_gif_frames_from_memory(const void* data, UINT dataSize, std::vector<AnimatedVisualFrame>& outFrames)
    {
        outFrames.clear();
        if (!data || dataSize == 0)
            return false;
        if (!ensure_gdiplus_started())
            return false;

        auto memory = GlobalAlloc(GMEM_MOVEABLE, dataSize);
        if (!memory)
            return false;

        auto* destination = GlobalLock(memory);
        if (!destination)
        {
            GlobalFree(memory);
            return false;
        }

        std::memcpy(destination, data, dataSize);
        GlobalUnlock(memory);

        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)) || !stream)
        {
            GlobalFree(memory);
            return false;
        }

        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream));
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
        {
            bitmap.reset();
            stream->Release();
            return false;
        }

        auto dimensionCount = bitmap->GetFrameDimensionsCount();
        if (dimensionCount == 0)
        {
            bitmap.reset();
            stream->Release();
            return false;
        }

        std::vector<GUID> dimensions(dimensionCount);
        if (bitmap->GetFrameDimensionsList(dimensions.data(), dimensionCount) != Gdiplus::Ok)
        {
            bitmap.reset();
            stream->Release();
            return false;
        }

        auto frameCount = bitmap->GetFrameCount(&dimensions[0]);
        if (frameCount == 0)
        {
            bitmap.reset();
            stream->Release();
            return false;
        }

        for (UINT i = 0; i < frameCount; ++i)
        {
            if (bitmap->SelectActiveFrame(&dimensions[0], i) != Gdiplus::Ok)
                continue;

            auto texture = create_texture_from_argb_bitmap(*bitmap);
            if (!texture)
                continue;

            outFrames.push_back({ texture, read_gif_frame_delay_ms(*bitmap, i) });
        }

        bitmap.reset();
        stream->Release();
        return !outFrames.empty();
    }

    LPDIRECT3DTEXTURE9 get_animated_texture_frame(const std::vector<AnimatedVisualFrame>& frames)
    {
        if (frames.empty())
            return nullptr;
        if (frames.size() == 1)
            return frames.front().texture;

        DWORD totalDelay = 0;
        for (const auto& frame : frames)
            totalDelay += frame.delayMs ? frame.delayMs : 100;

        if (totalDelay == 0)
            return frames.front().texture;

        auto position = GetTickCount() % totalDelay;
        DWORD accumulated = 0;
        for (const auto& frame : frames)
        {
            accumulated += frame.delayMs ? frame.delayMs : 100;
            if (position < accumulated)
                return frame.texture;
        }

        return frames.back().texture;
    }
}
