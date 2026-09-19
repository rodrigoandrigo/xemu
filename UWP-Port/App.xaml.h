//
// App.xaml.h
// Declaração da classe App.
//

#pragma once

#include "App.g.h"
#include "DirectXPage.xaml.h"

namespace UWP_Port
{
		/// <summary>
	/// Fornece o comportamento específico do aplicativo para complementar a classe Application padrão.
	/// </summary>
	ref class App sealed
	{
	public:
		App();
		virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

		// Releases currently-unused pages in this process's working set back
		// to the OS. Safe to call at any time, including mid-frame while
		// xemu is running; it never touches live emulator state. Exposed
		// static so any part of the app (render-loop stall detection,
		// periodic upkeep, memory-pressure callbacks) can request a trim.
		static void TrimWorkingSet();

	private:
		void ConfigureWindowBounds();
		void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
		void OnResuming(Platform::Object ^sender, Platform::Object ^args);
		void OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e);
		void OnAppMemoryUsageIncreased(Platform::Object^ sender, Platform::Object^ args);
		void OnAppMemoryUsageLimitChanging(Platform::Object^ sender, Windows::System::AppMemoryUsageLimitChangingEventArgs^ args);
		DirectXPage^ m_directXPage;
	};
}
