//
// DirectXPage.xaml.h
// Declaração da classe DirectXPage.
//

#pragma once

#include "DirectXPage.g.h"

#include "XemuHost.h"
#include "VLanManager.h"
#include <chrono>

namespace UWP_Port
{
	/// <summary>
	/// Uma página que hospeda um SwapChainPanel do DirectX.
	/// </summary>
	public ref class DirectXPage sealed
	{
	public:
		DirectXPage();
		virtual ~DirectXPage();

		void SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state);

	private:
		// Manipulador de eventos de renderização de baixo nível XAML.
		void OnRendering(Platform::Object^ sender, Platform::Object^ args);

		// Manipuladores de eventos da janela.
		void OnVisibilityChanged(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::VisibilityChangedEventArgs^ args);
		void OnBackRequested(Platform::Object^ sender,
		                     Windows::UI::Core::BackRequestedEventArgs^ args);
		void OnCoreKeyDown(Windows::UI::Core::CoreWindow^ sender,
		                   Windows::UI::Core::KeyEventArgs^ args);
		void OnCoreKeyUp(Windows::UI::Core::CoreWindow^ sender,
		                 Windows::UI::Core::KeyEventArgs^ args);
		void OnRenderPanelLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void OnRenderPanelSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ args);
		void OnRenderPanelScaleChanged(Windows::UI::Xaml::Controls::SwapChainPanel^ sender, Platform::Object^ args);
		void FocusEmulatorInput();
		void HideSystemPointer();
		void TrackRenderHealth(bool framePresented);
		void UpdateMemoryStatus();
		void UpdateStartButtonState();
		void RefreshLogView();

		// Outros manipuladores de eventos.
		void AppBarButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void NavigationButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void StartXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void PauseXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResumeXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void ResetXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void StopXemu_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SaveSettings_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SaveVlanSettings_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void CreateVlanRoom_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void UdpServer_SelectionChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void AutoSaveSettings_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void AutoSaveSettings_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
		void AutoSaveSettings_ValueChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e);
		void WireAutomaticSettings();
		void LoadSettings();
		bool SaveSettings(bool saveNetwork);
		void SelectFile_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void SelectFolder_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
		void MountXboxFile(Windows::Storage::StorageFile^ file,
		                   Platform::String^ tagValue, bool persist);
		void RestorePersistedFiles();
		void RestorePersistedFile(Platform::String^ tagValue);
		void MountXboxFolder(Windows::Storage::StorageFolder^ folder,
		                     Platform::String^ tagValue, bool persist);
		void RestorePersistedFolder(Platform::String^ tagValue);
		void MountDefaultGamesFolder();
		void PrepareLocalMachineFolder(Platform::String^ folderName,
		                               Platform::String^ tagValue);
		std::unique_ptr<XemuHost> m_xemu;
		std::unique_ptr<VLanManager> m_vlan;
		Windows::Foundation::EventRegistrationToken m_renderingToken;
		Windows::Foundation::EventRegistrationToken m_visibilityChangedToken;
		Windows::Foundation::EventRegistrationToken m_backRequestedToken;
		Windows::Foundation::EventRegistrationToken m_keyDownToken;
		Windows::Foundation::EventRegistrationToken m_keyUpToken;
		Windows::Foundation::EventRegistrationToken m_panelLoadedToken;
		Windows::Foundation::EventRegistrationToken m_panelSizeChangedToken;
		Windows::Foundation::EventRegistrationToken m_panelScaleChangedToken;
		bool m_windowVisible;
		bool m_renderAttached;
		bool m_flashReady;
		bool m_bootromReady;
		bool m_hddReady;
		bool m_dvdReady;
		bool m_flashMountPending;
		bool m_bootromMountPending;
		bool m_hddMountPending;
		Windows::UI::Core::CoreCursor^ m_savedSystemPointerCursor;
		bool m_systemPointerHidden;
		unsigned int m_logRefreshFrames;
		std::chrono::steady_clock::time_point m_lastPresentTime;
		std::chrono::steady_clock::time_point m_lastPeriodicTrim;
		bool m_stalled;
		std::string m_lastVlanStatus;
	};
}
