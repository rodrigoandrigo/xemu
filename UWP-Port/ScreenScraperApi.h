#pragma once

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace UWP_Port
{
    ref class ScreenScraperApi sealed
    {
    internal:
        ScreenScraperApi();

        void Configure(Platform::String^ developerId,
                       Platform::String^ developerPassword,
                       Platform::String^ softwareName,
                       Platform::String^ user,
                       Platform::String^ password);
        void SetRepository(Windows::Storage::StorageFolder^ folder, bool persist);
        void RestoreRepository();
        void ScrapeGame(unsigned int systemId, Platform::String^ gameName,
                        Platform::String^ region, bool downloadVideo);
        void Cancel();

        property Platform::String^ Status { Platform::String^ get(); }
        property Platform::String^ RepositoryPath { Platform::String^ get(); }
        property bool IsBusy { bool get(); }

    private:
        void SetStatus(Platform::String^ status);
        Platform::String^ BuildUrl(Platform::String^ endpoint,
                                   const std::vector<std::pair<Platform::String^,
                                   Platform::String^>>& parameters);
        void RunScrape(unsigned int generation, unsigned int systemId,
                       Platform::String^ gameName, Platform::String^ region,
                       bool downloadVideo);

        std::mutex m_mutex;
        Platform::String^ m_developerId;
        Platform::String^ m_developerPassword;
        Platform::String^ m_softwareName;
        Platform::String^ m_user;
        Platform::String^ m_password;
        Platform::String^ m_status;
        Windows::Storage::StorageFolder^ m_repository;
        std::atomic<unsigned int> m_generation;
        std::atomic_bool m_busy;
    };
}
