#include "pch.h"
#include "ScreenScraperApi.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Streams;
using namespace Windows::Web::Http;

namespace
{
String^ RepositoryToken() { return "screenscraper-media-root"; }

String^ SafeName(String^ value)
{
    std::wstring result = value ? value->Data() : L"game";
    for (wchar_t& c : result) {
        if (!(iswalnum(c) || c == L'-' || c == L'_' || c == L'.')) c = L'_';
    }
    while (!result.empty() && (result.back() == L'.' || result.back() == L' '))
        result.pop_back();
    if (result.empty()) result = L"game";
    if (result.size() > 96) result.resize(96);
    return ref new String(result.c_str());
}

JsonObject^ ObjectMember(JsonObject^ object, String^ name)
{
    if (!object || !object->HasKey(name)) return nullptr;
    try { return object->GetNamedObject(name); } catch (Exception^) { return nullptr; }
}

JsonArray^ ArrayMember(JsonObject^ object, String^ name)
{
    if (!object || !object->HasKey(name)) return nullptr;
    try { return object->GetNamedArray(name); } catch (Exception^) { return nullptr; }
}

String^ StringMember(JsonObject^ object, String^ name)
{
    if (!object || !object->HasKey(name)) return nullptr;
    try { return object->GetNamedString(name); } catch (Exception^) {
        try { return object->GetNamedNumber(name).ToString(); } catch (Exception^) { return nullptr; }
    }
}

JsonObject^ FirstGame(JsonObject^ root)
{
    JsonObject^ response = ObjectMember(root, "response");
    if (!response) response = root;
    if (auto game = ObjectMember(response, "jeu")) return game;
    if (auto games = ArrayMember(response, "jeux")) {
        if (games->Size) {
            try { return games->GetObjectAt(0); } catch (Exception^) {}
        }
    }
    return ObjectMember(response, "jeux");
}

bool IsPng(IBuffer^ buffer)
{
    if (!buffer || buffer->Length < 8) return false;
    Array<uint8_t>^ bytes;
    CryptographicBuffer::CopyToByteArray(buffer, &bytes);
    const uint8_t signature[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return memcmp(bytes->Data, signature, sizeof(signature)) == 0;
}

bool IsMp4(IBuffer^ buffer)
{
    if (!buffer || buffer->Length < 12) return false;
    Array<uint8_t>^ bytes;
    CryptographicBuffer::CopyToByteArray(buffer, &bytes);
    return memcmp(bytes->Data + 4, "ftyp", 4) == 0;
}

struct MediaChoice {
    String^ type;
    String^ region;
};

MediaChoice ChooseMedia(JsonObject^ game, const std::vector<String^>& types,
                        String^ preferredRegion)
{
    auto medias = ArrayMember(game, "medias");
    if (!medias) return {};
    std::vector<String^> regions{ preferredRegion, "wor", "us", "eu", "jp" };
    for (auto type : types) {
        MediaChoice fallback{};
        for (auto region : regions) {
            for (unsigned int index = 0; index < medias->Size; ++index) {
                JsonObject^ media = nullptr;
                try { media = medias->GetObjectAt(index); } catch (Exception^) { continue; }
                auto mediaType = StringMember(media, "type");
                auto parent = StringMember(media, "parent");
                auto mediaRegion = StringMember(media, "region");
                if (!mediaType || _wcsicmp(mediaType->Data(), type->Data()) != 0) continue;
                if (parent && _wcsicmp(parent->Data(), L"jeu") != 0) continue;
                if (!fallback.type) fallback = { mediaType, mediaRegion };
                if (region && mediaRegion &&
                    _wcsicmp(mediaRegion->Data(), region->Data()) == 0)
                    return { mediaType, mediaRegion };
            }
        }
        if (fallback.type) return fallback;
    }
    return {};
}
}

namespace UWP_Port
{
ScreenScraperApi::ScreenScraperApi()
    : m_status("Select a media repository"), m_repository(nullptr),
      m_generation(1), m_busy(false)
{
}

String^ ScreenScraperApi::Status::get()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

String^ ScreenScraperApi::RepositoryPath::get()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_repository ? m_repository->Path : "Not selected";
}

bool ScreenScraperApi::IsBusy::get() { return m_busy.load(); }

void ScreenScraperApi::SetStatus(String^ status)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = status;
}

void ScreenScraperApi::Configure(String^ developerId, String^ developerPassword,
                                 String^ softwareName, String^ user,
                                 String^ password)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_developerId = developerId;
    m_developerPassword = developerPassword;
    m_softwareName = softwareName;
    m_user = user;
    m_password = password;
}

void ScreenScraperApi::SetRepository(StorageFolder^ folder, bool persist)
{
    if (!folder) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_repository = folder;
        m_status = "Media repository ready";
    }
    if (persist)
        StorageApplicationPermissions::FutureAccessList->AddOrReplace(
            RepositoryToken(), folder);
}

void ScreenScraperApi::RestoreRepository()
{
    auto access = StorageApplicationPermissions::FutureAccessList;
    if (!access->ContainsItem(RepositoryToken())) return;
    create_task(access->GetFolderAsync(RepositoryToken())).then(
        [this](task<StorageFolder^> result) {
            try { SetRepository(result.get(), false); }
            catch (Exception^) {
                StorageApplicationPermissions::FutureAccessList->Remove(RepositoryToken());
                SetStatus("The saved media repository is no longer accessible");
            }
        });
}

String^ ScreenScraperApi::BuildUrl(String^ endpoint,
    const std::vector<std::pair<String^, String^>>& parameters)
{
    std::vector<std::pair<String^, String^>> all;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        all = { { "devid", m_developerId }, { "devpassword", m_developerPassword },
                { "softname", m_softwareName }, { "ssid", m_user },
                { "sspassword", m_password } };
    }
    all.insert(all.end(), parameters.begin(), parameters.end());
    String^ url = "https://api.screenscraper.fr/api2/" + endpoint + "?";
    bool first = true;
    for (auto& parameter : all) {
        if (!parameter.second || parameter.second->IsEmpty()) continue;
        if (!first) url += "&";
        first = false;
        url += Uri::EscapeComponent(parameter.first) + "=" +
               Uri::EscapeComponent(parameter.second);
    }
    return url;
}

void ScreenScraperApi::Cancel()
{
    ++m_generation;
    m_busy = false;
    SetStatus("ScreenScraper operation cancelled");
}

void ScreenScraperApi::ScrapeGame(unsigned int systemId, String^ gameName,
                                  String^ region, bool downloadVideo)
{
    if (m_busy.exchange(true)) {
        SetStatus("A ScreenScraper operation is already running");
        return;
    }
    StorageFolder^ repository = nullptr;
    String^ developer = nullptr;
    String^ software = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        repository = m_repository;
        developer = m_developerId;
        software = m_softwareName;
    }
    if (!repository || !developer || developer->IsEmpty() || !software ||
        software->IsEmpty() || !systemId || !gameName || gameName->IsEmpty()) {
        m_busy = false;
        SetStatus("Repository, developer identity, system ID and game name are required");
        return;
    }
    unsigned int generation = ++m_generation;
    SetStatus("Searching ScreenScraper...");
    RunScrape(generation, systemId, gameName,
              region && !region->IsEmpty() ? region : "wor", downloadVideo);
}

void ScreenScraperApi::RunScrape(unsigned int generation, unsigned int systemId,
                                 String^ gameName, String^ region,
                                 bool downloadVideo)
{
    create_task([this, generation, systemId, gameName, region, downloadVideo]() {
        try {
            auto checkCancelled = [this, generation]() {
                if (generation != m_generation.load())
                    throw ref new OperationCanceledException();
            };
            auto client = ref new HttpClient();
            String^ system = systemId.ToString();
            auto searchUrl = BuildUrl("jeuRecherche.php", {
                { "output", "json" }, { "systemeid", system },
                { "recherche", gameName }
            });
            auto searchText = create_task(client->GetStringAsync(
                ref new Uri(searchUrl))).get();
            checkCancelled();
            auto searchGame = FirstGame(JsonObject::Parse(searchText));
            auto gameId = StringMember(searchGame, "id");
            if (!gameId) gameId = StringMember(searchGame, "idjeu");
            if (!gameId) throw ref new FailureException(
                "No ScreenScraper match was found");

            SetStatus("Loading game metadata...");
            auto infoUrl = BuildUrl("jeuInfos.php", {
                { "output", "json" }, { "systemeid", system },
                { "gameid", gameId }
            });
            auto json = create_task(client->GetStringAsync(ref new Uri(infoUrl))).get();
            checkCancelled();
            auto game = FirstGame(JsonObject::Parse(json));
            if (!game) throw ref new FailureException(
                "ScreenScraper returned no game data");

            StorageFolder^ repository;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                repository = m_repository;
            }
            auto directory = SafeName(gameName);
            auto folder = create_task(repository->CreateFolderAsync(directory,
                CreationCollisionOption::OpenIfExists)).get();
            auto metadataFile = create_task(folder->CreateFileAsync("metadata.json",
                CreationCollisionOption::ReplaceExisting)).get();
            create_task(FileIO::WriteTextAsync(metadataFile, json)).get();

            struct Request { String^ file; std::vector<String^> types; bool video; };
            std::vector<Request> requests{
                { "box2d.png", { "box-2D" }, false },
                { "box3d.png", { "box-3D" }, false },
                { "fanart.png", { "fanart" }, false },
                { "screenshot.png", { "ss" }, false },
                { "logo.png", { "wheel-hd", "wheel" }, false },
            };
            if (downloadVideo) requests.push_back(
                { "video.mp4", { "video" }, true });

            auto saved = ref new JsonArray();
            for (auto& request : requests) {
                checkCancelled();
                auto choice = ChooseMedia(game, request.types, region);
                if (!choice.type) continue;
                auto media = request.video ? choice.type : choice.type +
                    (choice.region && !choice.region->IsEmpty() ?
                     "(" + choice.region + ")" : "");
                std::vector<std::pair<String^, String^>> parameters{
                    { "systemeid", system }, { "jeuid", gameId },
                    { "media", media }
                };
                if (!request.video)
                    parameters.push_back({ "outputformat", "png" });
                auto endpoint = request.video ?
                    ref new String(L"mediaVideoJeu.php") :
                    ref new String(L"mediaJeu.php");
                auto url = BuildUrl(endpoint, parameters);
                SetStatus("Downloading " + request.file + "...");
                auto buffer = create_task(client->GetBufferAsync(ref new Uri(url))).get();
                if (!(request.video ? IsMp4(buffer) : IsPng(buffer))) continue;
                auto file = create_task(folder->CreateFileAsync(request.file,
                    CreationCollisionOption::ReplaceExisting)).get();
                create_task(FileIO::WriteBufferAsync(file, buffer)).get();
                saved->Append(JsonValue::CreateStringValue(request.file));
            }

            auto manifest = ref new JsonObject();
            manifest->SetNamedValue("schemaVersion", JsonValue::CreateNumberValue(1));
            manifest->SetNamedValue("gameId", JsonValue::CreateStringValue(gameId));
            manifest->SetNamedValue("systemId", JsonValue::CreateNumberValue(systemId));
            manifest->SetNamedValue("name", JsonValue::CreateStringValue(gameName));
            manifest->SetNamedValue("saved", saved);
            auto manifestFile = create_task(folder->CreateFileAsync("media.json",
                CreationCollisionOption::ReplaceExisting)).get();
            create_task(FileIO::WriteTextAsync(manifestFile,
                manifest->Stringify())).get();

            auto catalog = ref new JsonObject();
            try {
                auto existing = create_task(repository->GetFileAsync("catalog.json")).get();
                catalog = JsonObject::Parse(
                    create_task(FileIO::ReadTextAsync(existing)).get());
            } catch (Exception^) {
                catalog = ref new JsonObject();
            }
            catalog->SetNamedValue("schemaVersion", JsonValue::CreateNumberValue(1));
            auto entry = ref new JsonObject();
            entry->SetNamedValue("name", JsonValue::CreateStringValue(gameName));
            entry->SetNamedValue("gameId", JsonValue::CreateStringValue(gameId));
            entry->SetNamedValue("systemId", JsonValue::CreateNumberValue(systemId));
            entry->SetNamedValue("directory", JsonValue::CreateStringValue(directory));
            auto games = ArrayMember(catalog, "games");
            if (!games) games = ref new JsonArray();
            for (int index = static_cast<int>(games->Size) - 1; index >= 0; --index) {
                try {
                    auto current = games->GetObjectAt(index);
                    auto currentDirectory = StringMember(current, "directory");
                    if (currentDirectory && _wcsicmp(currentDirectory->Data(),
                                                     directory->Data()) == 0)
                        games->RemoveAt(index);
                } catch (Exception^) {
                }
            }
            games->Append(entry);
            catalog->SetNamedValue("games", games);
            auto catalogFile = create_task(repository->CreateFileAsync("catalog.json",
                CreationCollisionOption::ReplaceExisting)).get();
            create_task(FileIO::WriteTextAsync(catalogFile,
                catalog->Stringify())).get();
            checkCancelled();
            m_busy = false;
            SetStatus("ScreenScraper media saved successfully");
        } catch (OperationCanceledException^) {
        } catch (Exception^ exception) {
            if (generation == m_generation.load()) {
                m_busy = false;
                SetStatus("ScreenScraper failed: " + exception->Message);
            }
        }
    });
}
}
