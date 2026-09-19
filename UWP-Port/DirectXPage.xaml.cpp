//
// DirectXPage.xaml.cpp
// Implementação da classe DirectXPage.
//

#include "pch.h"
#include "DirectXPage.xaml.h"

#include <cmath>
#include <sstream>

using namespace UWP_Port;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Graphics::Display;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace concurrency;

namespace
{
IPropertySet^ SettingsValues()
{
	return ApplicationData::Current->LocalSettings->Values;
}

bool ReadBool(String^ key, bool fallback)
{
	auto values = SettingsValues();
	if (!values->HasKey(key)) return fallback;
	try {
		return safe_cast<bool>(values->Lookup(key));
	} catch (Platform::Exception^) {
		return fallback;
	}
}

int ReadInt(String^ key, int fallback)
{
	auto values = SettingsValues();
	if (!values->HasKey(key)) return fallback;
	try {
		return safe_cast<int>(values->Lookup(key));
	} catch (Platform::Exception^) {
		return fallback;
	}
}

double ReadDouble(String^ key, double fallback)
{
	auto values = SettingsValues();
	if (!values->HasKey(key)) return fallback;
	try {
		return safe_cast<double>(values->Lookup(key));
	} catch (Platform::Exception^) {
		return fallback;
	}
}

String^ ReadString(String^ key, String^ fallback)
{
	auto values = SettingsValues();
	if (!values->HasKey(key)) return fallback;
	try {
		return safe_cast<String^>(values->Lookup(key));
	} catch (Platform::Exception^) {
		return fallback;
	}
}

int ClampIndex(int value, int count, int fallback)
{
	return value >= 0 && value < count ? value : fallback;
}

double ClampValue(double value, double minimum, double maximum, double fallback)
{
	return std::isfinite(value) && value >= minimum && value <= maximum ?
		value : fallback;
}

std::string Utf8(String^ value)
{
	if (!value || value->IsEmpty()) {
		return {};
	}
	int size = WideCharToMultiByte(CP_UTF8, 0, value->Data(), value->Length(),
	                               nullptr, 0, nullptr, nullptr);
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value->Data(), value->Length(),
	                    &result[0], size, nullptr, nullptr);
	return result;
}

std::string TomlString(String^ value)
{
	std::string input = Utf8(value);
	std::string output = "\"";
	for (char c : input) {
		if (c == '\\' || c == '"') output += '\\';
		output += c;
	}
	return output + "\"";
}

const char *BoolText(bool value)
{
	return value ? "true" : "false";
}
}

DirectXPage::DirectXPage():
	m_windowVisible(true),
	m_renderAttached(false),
	m_flashReady(false),
	m_bootromReady(false),
	m_hddReady(false),
	m_dvdReady(false),
	m_flashMountPending(false),
	m_bootromMountPending(false),
	m_hddMountPending(false),
	m_savedSystemPointerCursor(nullptr),
	m_systemPointerHidden(false),
	m_logRefreshFrames(0),
	m_lastPresentTime(std::chrono::steady_clock::now()),
	m_lastPeriodicTrim(std::chrono::steady_clock::now()),
	m_stalled(false)
{
	InitializeComponent();

	// Registre manipuladores de eventos para o ciclo de vida da página.
	CoreWindow^ window = Window::Current->CoreWindow;
	WeakReference weakThis(this);

	m_visibilityChangedToken = window->VisibilityChanged +=
		ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(
			[weakThis](CoreWindow^ sender, VisibilityChangedEventArgs^ args) {
				auto page = weakThis.Resolve<DirectXPage>();
				if (page) page->OnVisibilityChanged(sender, args);
			});
	m_backRequestedToken = SystemNavigationManager::GetForCurrentView()->BackRequested +=
		ref new EventHandler<BackRequestedEventArgs^>(
			[weakThis](Object^ sender, BackRequestedEventArgs^ args) {
				auto page = weakThis.Resolve<DirectXPage>();
				if (page) page->OnBackRequested(sender, args);
			});
	m_keyDownToken = window->KeyDown +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			[weakThis](CoreWindow^ sender, KeyEventArgs^ args) {
				auto page = weakThis.Resolve<DirectXPage>();
				if (page) page->OnCoreKeyDown(sender, args);
			});
	m_keyUpToken = window->KeyUp +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			[weakThis](CoreWindow^ sender, KeyEventArgs^ args) {
				auto page = weakThis.Resolve<DirectXPage>();
				if (page) page->OnCoreKeyUp(sender, args);
			});

	m_xemu = std::unique_ptr<XemuHost>(new XemuHost());
	m_vlan = std::unique_ptr<VLanManager>(new VLanManager());
	LoadSettings();
	WireAutomaticSettings();
	RestorePersistedFiles();
	m_panelLoadedToken = swapChainPanel->Loaded += ref new RoutedEventHandler(
		[weakThis](Object^ sender, RoutedEventArgs^ args) {
			auto page = weakThis.Resolve<DirectXPage>();
			if (page) page->OnRenderPanelLoaded(sender, args);
		});
	m_panelSizeChangedToken = swapChainPanel->SizeChanged += ref new SizeChangedEventHandler(
		[weakThis](Object^ sender, SizeChangedEventArgs^ args) {
			auto page = weakThis.Resolve<DirectXPage>();
			if (page) page->OnRenderPanelSizeChanged(sender, args);
		});
	m_panelScaleChangedToken = swapChainPanel->CompositionScaleChanged +=
		ref new TypedEventHandler<SwapChainPanel^, Object^>(
			[weakThis](SwapChainPanel^ sender, Object^ args) {
				auto page = weakThis.Resolve<DirectXPage>();
				if (page) page->OnRenderPanelScaleChanged(sender, args);
			});
	m_renderingToken = Windows::UI::Xaml::Media::CompositionTarget::Rendering +=
		ref new EventHandler<Object^>([weakThis](Object^ sender, Object^ args) {
			auto page = weakThis.Resolve<DirectXPage>();
			if (page) page->OnRendering(sender, args);
		});
}

DirectXPage::~DirectXPage()
{
	if (m_vlan) m_vlan->Stop();
	// Interrompa a renderização e o processamento de eventos em destruição.
	Windows::UI::Xaml::Media::CompositionTarget::Rendering -= m_renderingToken;
	SystemNavigationManager::GetForCurrentView()->BackRequested -=
		m_backRequestedToken;
	auto window = Window::Current->CoreWindow;
	window->VisibilityChanged -= m_visibilityChangedToken;
	window->KeyDown -= m_keyDownToken;
	window->KeyUp -= m_keyUpToken;
	swapChainPanel->Loaded -= m_panelLoadedToken;
	swapChainPanel->SizeChanged -= m_panelSizeChangedToken;
	swapChainPanel->CompositionScaleChanged -= m_panelScaleChangedToken;
	m_xemu->Stop();
	if (m_systemPointerHidden) {
		Window::Current->CoreWindow->PointerCursor = m_savedSystemPointerCursor;
	}
}

void DirectXPage::OnRendering(Object^, Object^)
{
	if (++m_logRefreshFrames >= 60) {
		m_logRefreshFrames = 0;
		if (toolTabs->SelectedIndex == 6) RefreshLogView();
		UpdateMemoryStatus();
		if (m_vlan && m_vlan->IsRunning()) {
			std::string status = m_vlan->Status();
			if (status != m_lastVlanStatus) {
				m_lastVlanStatus = status;
				vlanStatus->Text = ref new String(
					std::wstring(status.begin(), status.end()).c_str());
			}
		}
	}
	if (m_windowVisible && m_xemu) {
		if (m_xemu->IsRunning()) {
			HideSystemPointer();
		}
		TrackRenderHealth(m_xemu->RenderFrame());
	}
}

namespace
{
// How long RenderFrame() can go without successfully presenting a frame
// before it counts as a stall. This is well above a normal frame budget
// (16-33ms), so only genuine hitches/hangs trigger it, not ordinary
// frame-pacing jitter.
constexpr double kStallThresholdSeconds = 1.5;

// Even when nothing stalls, periodically hand back whatever working set
// xemu has accumulated (shader cache, JIT code cache) so a long play
// session doesn't sit on memory the OS could be giving to other apps.
constexpr double kPeriodicTrimIntervalSeconds = 30.0;
}

void DirectXPage::TrackRenderHealth(bool framePresented)
{
	auto now = std::chrono::steady_clock::now();

	if (!m_xemu->IsRunning()) {
		m_lastPresentTime = now;
		m_stalled = false;
		return;
	}

	if (framePresented) {
		m_lastPresentTime = now;
		m_stalled = false;
	} else {
		auto sinceLastPresent =
			std::chrono::duration<double>(now - m_lastPresentTime).count();
		if (!m_stalled && sinceLastPresent >= kStallThresholdSeconds) {
			// We've gone noticeably long without presenting a frame.
			// There's no shader/texture-cache-clear entry point exposed
			// by the embedded DLL, so the concrete, safe thing this
			// layer can do is give back whatever reclaimable memory the
			// process is holding, in case the stall is caused by memory
			// pressure (e.g. the OS throttling the app near its quota).
			m_stalled = true;
			m_lastPeriodicTrim = now;
			App::TrimWorkingSet();
		}
	}

	auto sincePeriodicTrim =
		std::chrono::duration<double>(now - m_lastPeriodicTrim).count();
	if (sincePeriodicTrim >= kPeriodicTrimIntervalSeconds) {
		m_lastPeriodicTrim = now;
		App::TrimWorkingSet();
	}
}

// Surfaces the OS-reported UWP memory quota directly, rather than a value
// this app computed itself. On Xbox this quota is a hard platform ceiling
// (Microsoft docs: 1 GB foreground for apps, 5 GB for games) that nothing
// in this project can raise; showing the live number lets you confirm
// exactly what ceiling you're actually up against and how close to it a
// given title/setting combination runs.
void DirectXPage::UpdateMemoryStatus()
{
	auto usage = Windows::System::MemoryManager::AppMemoryUsage;
	auto limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
	auto level = Windows::System::MemoryManager::AppMemoryUsageLevel;

	const wchar_t* levelText = L"LOW";
	switch (level) {
	case Windows::System::AppMemoryUsageLevel::Medium:    levelText = L"MEDIUM";     break;
	case Windows::System::AppMemoryUsageLevel::High:      levelText = L"HIGH";       break;
	case Windows::System::AppMemoryUsageLevel::OverLimit: levelText = L"OVER LIMIT"; break;
	default: break;
	}

	wchar_t text[96];
	swprintf_s(text, L"Memory: %.0f / %.0f MB (%s)",
	           usage / (1024.0 * 1024.0), limit / (1024.0 * 1024.0), levelText);
	memoryStatus->Text = ref new String(text);
}

// Salva o estado atual do aplicativo para eventos de suspensão e de encerramento.
void DirectXPage::SaveInternalState(IPropertySet^ state)
{
	m_xemu->Pause();

	// Coloque aqui o código para salvar o estado do aplicativo.
}

// Carrega o estado atual do aplicativo para eventos de retomada.
void DirectXPage::LoadInternalState(IPropertySet^ state)
{
	// Coloque aqui o código para carregar o estado do aplicativo.

	m_xemu->Resume();
}

// Manipuladores de eventos da janela.

void DirectXPage::OnVisibilityChanged(CoreWindow^ sender, VisibilityChangedEventArgs^ args)
{
	m_windowVisible = args->Visible;
}

void DirectXPage::OnBackRequested(Object^, BackRequestedEventArgs^ args)
{
	args->Handled = true;
}

void DirectXPage::OnCoreKeyDown(CoreWindow^, KeyEventArgs^ args)
{
	if (args->VirtualKey == Windows::System::VirtualKey::GamepadB) {
		args->Handled = true;
	}
}

void DirectXPage::OnCoreKeyUp(CoreWindow^, KeyEventArgs^ args)
{
	if (args->VirtualKey == Windows::System::VirtualKey::GamepadB) {
		args->Handled = true;
	}
}

void DirectXPage::OnRenderPanelLoaded(Object^, RoutedEventArgs^)
{
	if (!m_renderAttached && m_xemu->AttachRenderPanel(swapChainPanel)) {
		m_renderAttached = true;
		m_xemu->UpdateRenderPanelSize(swapChainPanel);
	} else if (!m_renderAttached) {
		auto error = m_xemu->LastError();
		errorText->Text = ref new String(
			std::wstring(error.begin(), error.end()).c_str());
	}
}

void DirectXPage::OnRenderPanelSizeChanged(Object^, SizeChangedEventArgs^)
{
	if (!m_renderAttached && swapChainPanel->IsLoaded) {
		OnRenderPanelLoaded(nullptr, nullptr);
	} else if (m_renderAttached) {
		m_xemu->UpdateRenderPanelSize(swapChainPanel);
	}
}

void DirectXPage::OnRenderPanelScaleChanged(SwapChainPanel^, Object^)
{
	if (m_renderAttached) {
		m_xemu->UpdateRenderPanelSize(swapChainPanel);
	}
}

// Chamado quando você clica no botão da barra de aplicativos.
void DirectXPage::AppBarButton_Click(Object^ sender, RoutedEventArgs^ e)
{
	// Use a barra de aplicativos se ela for apropriada para seu aplicativo. Crie a barra de aplicativos, 
	// depois preencha os manipuladores de eventos (como este).
}

void DirectXPage::NavigationButton_Click(Object^ sender, RoutedEventArgs^)
{
	toolTabs->SelectedIndex = _wtoi(safe_cast<Button^>(sender)->Tag->ToString()->Data());
	if (toolTabs->SelectedIndex == 1 && !m_xemu->IsRunning()) {
		/* Files may be copied into LocalState through Device Portal while the
		 * application is open. Refresh the default machine folders whenever the
		 * Files page is opened so those files become immediately available. */
		PrepareLocalMachineFolder("BIOS", "flash");
		PrepareLocalMachineFolder("MCPX", "bootrom");
		PrepareLocalMachineFolder("hard_disk", "hdd");
	} else if (toolTabs->SelectedIndex == 6) {
		m_logRefreshFrames = 0;
		RefreshLogView();
	}
}

void DirectXPage::RefreshLogView()
{
	auto path = ApplicationData::Current->LocalFolder->Path + "\\xemu.log";
	CREATEFILE2_EXTENDED_PARAMETERS parameters = {};
	parameters.dwSize = sizeof(parameters);
	HANDLE file = CreateFile2(path->Data(), GENERIC_READ,
	                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                          OPEN_EXISTING, &parameters);
	if (file == INVALID_HANDLE_VALUE) {
		logText->Text = "xemu.log has not been created yet.";
		return;
	}

	LARGE_INTEGER fileSize = {};
	if (!GetFileSizeEx(file, &fileSize)) {
		CloseHandle(file);
		logText->Text = "Unable to read xemu.log.";
		return;
	}

	constexpr DWORD maxLogBytes = 256 * 1024;
	DWORD bytesToRead = fileSize.QuadPart < maxLogBytes ?
		static_cast<DWORD>(fileSize.QuadPart) : maxLogBytes;
	if (fileSize.QuadPart > bytesToRead) {
		LARGE_INTEGER offset = {};
		offset.QuadPart = fileSize.QuadPart - bytesToRead;
		SetFilePointerEx(file, offset, nullptr, FILE_BEGIN);
	}

	std::string content(bytesToRead, '\0');
	DWORD bytesRead = 0;
	bool read = bytesToRead == 0 ||
		ReadFile(file, &content[0], bytesToRead, &bytesRead, nullptr);
	CloseHandle(file);
	if (!read) {
		logText->Text = "Unable to read xemu.log.";
		return;
	}
	content.resize(bytesRead);
	if (fileSize.QuadPart > bytesToRead) {
		auto firstLine = content.find('\n');
		if (firstLine != std::string::npos) content.erase(0, firstLine + 1);
	}
	if (content.empty()) {
		logText->Text = "xemu.log is empty.";
		return;
	}

	int length = MultiByteToWideChar(CP_UTF8, 0, content.data(),
	                                 static_cast<int>(content.size()),
	                                 nullptr, 0);
	if (length <= 0) {
		logText->Text = "Unable to decode xemu.log.";
		return;
	}
	std::wstring text(length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, content.data(),
	                    static_cast<int>(content.size()), &text[0], length);
	logText->Text = ref new String(text.c_str(), static_cast<unsigned int>(text.size()));
}

void DirectXPage::FocusEmulatorInput()
{
	startButton->IsTabStop = false;
	launcherPanel->IsHitTestVisible = false;
	this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void DirectXPage::HideSystemPointer()
{
	try {
		auto coreWindow = Window::Current->CoreWindow;
		if (!m_systemPointerHidden) {
			m_savedSystemPointerCursor = coreWindow->PointerCursor;
			m_systemPointerHidden = true;
		}
		if (coreWindow->PointerCursor != nullptr) {
			coreWindow->PointerCursor = nullptr;
		}
	} catch (Platform::Exception^) {
	}
}

void DirectXPage::UpdateStartButtonState()
{
	bool ready = m_flashReady && m_bootromReady && m_hddReady;
	startButton->IsEnabled = ready;
	requiredFilesStatus->Text = ready ?
		"Required files are ready. DVD/XISO is optional." :
		"Select BIOS, MCPX, and hard disk to start. DVD/XISO is optional.";
	requiredFilesStatus->Foreground = ref new SolidColorBrush(
		ready ? Windows::UI::ColorHelper::FromArgb(255, 76, 195, 138) :
		        Windows::UI::ColorHelper::FromArgb(255, 255, 200, 87));
}

void DirectXPage::StartXemu_Click(Object^, RoutedEventArgs^)
{
	if (!startButton->IsEnabled) {
		return;
	}
	if (!SaveSettings(false)) {
		toolTabs->SelectedIndex = 6;
		return;
	}
	if (vlanEnabled->IsChecked->Value &&
	    (!VLanManager::IsValidRoomCode(Utf8(vlanRoomCode->Text)) ||
	     !m_vlan->Start(Utf8(vlanCoordinator->Text), Utf8(vlanRoomCode->Text)))) {
		errorText->Text = "VLan/VPN requires a valid coordinator and 32-character room code.";
		return;
	}
	if (m_xemu->Start()) { hostStatus->Text = "RUNNING"; m_lastPresentTime = std::chrono::steady_clock::now(); m_lastPeriodicTrim = m_lastPresentTime; m_stalled = false; FocusEmulatorInput(); launcherPanel->Visibility = Windows::UI::Xaml::Visibility::Collapsed; HideSystemPointer(); }
	else { if (m_vlan) m_vlan->Stop(); auto e = m_xemu->LastError(); errorText->Text = ref new String(std::wstring(e.begin(), e.end()).c_str()); }
}

void DirectXPage::PauseXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Pause(); hostStatus->Text = "PAUSED"; }
void DirectXPage::ResumeXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Resume(); hostStatus->Text = "RUNNING"; }
void DirectXPage::ResetXemu_Click(Object^, RoutedEventArgs^) { m_xemu->Reset(); }
void DirectXPage::StopXemu_Click(Object^, RoutedEventArgs^)
{
	m_xemu->Shutdown();
	if (m_vlan) {
		m_vlan->Stop();
	}
}

void DirectXPage::LoadSettings()
{
	skipBootAnimation->IsChecked = ReadBool("general.skip_boot_anim", false);
	hardFpu->IsChecked = ReadBool("perf.hard_fpu", true);
	cacheShaders->IsChecked = ReadBool("perf.cache_shaders", true);
	filterSnapshots->IsChecked = ReadBool("general.snapshots.filter_current_game", false);
	autoBind->IsChecked = ReadBool("input.auto_bind", true);
	invertLeftX->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_left_x", false);
	invertLeftY->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_left_y", false);
	invertRightX->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_right_x", false);
	invertRightY->IsChecked = ReadBool("input.uwp_gamepad.invert_axis_right_y", false);
	port1Driver->SelectedIndex = ClampIndex(ReadInt("input.port1.driver", 0), 2, 0);
	port2Driver->SelectedIndex = ClampIndex(ReadInt("input.port2.driver", 0), 2, 0);
	port3Driver->SelectedIndex = ClampIndex(ReadInt("input.port3.driver", 0), 2, 0);
	port4Driver->SelectedIndex = ClampIndex(ReadInt("input.port4.driver", 0), 2, 0);
	port1SlotA->SelectedIndex = ClampIndex(ReadInt("input.port1.slot_a", 0), 2, 0);
	port1SlotB->SelectedIndex = ClampIndex(ReadInt("input.port1.slot_b", 0), 2, 0);
	port2SlotA->SelectedIndex = ClampIndex(ReadInt("input.port2.slot_a", 0), 2, 0);
	port2SlotB->SelectedIndex = ClampIndex(ReadInt("input.port2.slot_b", 0), 2, 0);
	port3SlotA->SelectedIndex = ClampIndex(ReadInt("input.port3.slot_a", 0), 2, 0);
	port3SlotB->SelectedIndex = ClampIndex(ReadInt("input.port3.slot_b", 0), 2, 0);
	port4SlotA->SelectedIndex = ClampIndex(ReadInt("input.port4.slot_a", 0), 2, 0);
	port4SlotB->SelectedIndex = ClampIndex(ReadInt("input.port4.slot_b", 0), 2, 0);
	surfaceScale->SelectedIndex = ClampIndex(ReadInt("display.quality.surface_scale", 1) - 1, 6, 0);
	filtering->SelectedIndex = ClampIndex(ReadInt("display.filtering", 0), 2, 0);
	displayFit->SelectedIndex = ClampIndex(ReadInt("display.ui.fit", 1), 3, 1);
	aspectRatio->SelectedIndex = ClampIndex(ReadInt("display.ui.aspect_ratio", 1), 4, 1);
	vsync->IsChecked = ReadBool("display.window.vsync", true);
	showNotifications->IsChecked = ReadBool("display.ui.show_notifications", true);
	useAnimations->IsChecked = ReadBool("display.ui.use_animations", true);
	autoUiScale->IsChecked = ReadBool("display.ui.auto_scale", true);
	uiScale->Value = ClampValue(ReadDouble("display.ui.scale", 1.0), 1.0, 3.0, 1.0);
	uiScale->IsEnabled = !autoUiScale->IsChecked->Value;
	useDsp->IsChecked = ReadBool("audio.use_dsp", false);
	useHrtf->IsChecked = ReadBool("audio.hrtf", true);
	volumeLimit->Value = ClampValue(ReadDouble("audio.volume_limit", 1.0), 0.0, 1.0, 1.0);
	networkEnabled->IsChecked = ReadBool("net.enable", true);
	networkBackend->SelectedIndex = ClampIndex(ReadInt("net.backend", 0), 2, 0);
	udpBindAddress->Text = ReadString("net.udp.bind_addr", "0.0.0.0:9368");
	udpServer->SelectedIndex = ClampIndex(ReadInt("net.udp.server", 0), 4, 0);
	udpRemoteAddress->Text = udpServer->SelectedIndex == 0 ?
		ReadString("net.udp.remote_addr", "") : "";
	udpRemoteAddress->IsEnabled = udpServer->SelectedIndex == 0;
	natForwardPorts->Text = ReadString("net.nat.forward_ports", "");
	vlanEnabled->IsChecked = ReadBool("net.vlan.enabled", false);
	vlanRole->SelectedIndex = ClampIndex(ReadInt("net.vlan.role", 0), 2, 0);
	vlanCoordinator->Text = ReadString("net.vlan.coordinator", "");
	vlanRoomCode->Text = ReadString("net.vlan.room_code", "");
	memoryLimit->SelectedIndex = ClampIndex(ReadInt("sys.mem_limit", 0), 2, 0);
	avPack->SelectedIndex = ClampIndex(ReadInt("sys.avpack", 1), 7, 1);
}

void DirectXPage::SaveSettings_Click(Object^, RoutedEventArgs^)
{
	if (SaveSettings(true)) {
		hostStatus->Text = "NETWORK SETTINGS SAVED";
	}
}

void DirectXPage::SaveVlanSettings_Click(Object^, RoutedEventArgs^)
{
	if (SaveSettings(true)) vlanStatus->Text = "VLan/VPN settings saved";
}

void DirectXPage::CreateVlanRoom_Click(Object^, RoutedEventArgs^)
{
	std::string code = VLanManager::CreateRoomCode();
	vlanRoomCode->Text = ref new String(
		std::wstring(code.begin(), code.end()).c_str());
	vlanRole->SelectedIndex = 0;
	vlanEnabled->IsChecked = true;
}

void DirectXPage::UdpServer_SelectionChanged(Object^, SelectionChangedEventArgs^)
{
	if (udpRemoteAddress == nullptr) {
		return;
	}

	bool manualAddress = udpServer->SelectedIndex <= 0;
	udpRemoteAddress->IsEnabled = manualAddress;
	if (!manualAddress) {
		udpRemoteAddress->Text = "";
	}
}

void DirectXPage::AutoSaveSettings_Click(Object^ sender, RoutedEventArgs^)
{
	if (sender == autoUiScale) {
		uiScale->IsEnabled = !autoUiScale->IsChecked->Value;
	}
	SaveSettings(false);
}

void DirectXPage::AutoSaveSettings_SelectionChanged(
	Object^, SelectionChangedEventArgs^)
{
	SaveSettings(false);
}

void DirectXPage::AutoSaveSettings_ValueChanged(
	Object^, RangeBaseValueChangedEventArgs^)
{
	SaveSettings(false);
}

void DirectXPage::WireAutomaticSettings()
{
	WeakReference weakThis(this);
	auto click = ref new RoutedEventHandler(
		[weakThis](Object^ sender, RoutedEventArgs^ args) {
			auto page = weakThis.Resolve<DirectXPage>();
			if (page) page->AutoSaveSettings_Click(sender, args);
		});
	skipBootAnimation->Click += click;
	hardFpu->Click += click;
	cacheShaders->Click += click;
	filterSnapshots->Click += click;
	autoBind->Click += click;
	invertLeftX->Click += click;
	invertLeftY->Click += click;
	invertRightX->Click += click;
	invertRightY->Click += click;
	vsync->Click += click;
	showNotifications->Click += click;
	useAnimations->Click += click;
	autoUiScale->Click += click;
	useDsp->Click += click;
	useHrtf->Click += click;

	auto selectionChanged = ref new SelectionChangedEventHandler(
		[weakThis](Object^ sender, SelectionChangedEventArgs^ args) {
			auto page = weakThis.Resolve<DirectXPage>();
			if (page) page->AutoSaveSettings_SelectionChanged(sender, args);
		});
	port1Driver->SelectionChanged += selectionChanged;
	port2Driver->SelectionChanged += selectionChanged;
	port3Driver->SelectionChanged += selectionChanged;
	port4Driver->SelectionChanged += selectionChanged;
	port1SlotA->SelectionChanged += selectionChanged;
	port1SlotB->SelectionChanged += selectionChanged;
	port2SlotA->SelectionChanged += selectionChanged;
	port2SlotB->SelectionChanged += selectionChanged;
	port3SlotA->SelectionChanged += selectionChanged;
	port3SlotB->SelectionChanged += selectionChanged;
	port4SlotA->SelectionChanged += selectionChanged;
	port4SlotB->SelectionChanged += selectionChanged;
	surfaceScale->SelectionChanged += selectionChanged;
	filtering->SelectionChanged += selectionChanged;
	displayFit->SelectionChanged += selectionChanged;
	aspectRatio->SelectionChanged += selectionChanged;
	memoryLimit->SelectionChanged += selectionChanged;
	avPack->SelectionChanged += selectionChanged;

	auto valueChanged = ref new RangeBaseValueChangedEventHandler(
		[weakThis](Object^ sender, RangeBaseValueChangedEventArgs^ args) {
			auto page = weakThis.Resolve<DirectXPage>();
			if (page) page->AutoSaveSettings_ValueChanged(sender, args);
		});
	uiScale->ValueChanged += valueChanged;
	volumeLimit->ValueChanged += valueChanged;
}

bool DirectXPage::SaveSettings(bool saveNetwork)
{
	auto values = SettingsValues();
	bool networkEnabledValue = saveNetwork ?
		(vlanEnabled->IsChecked->Value || networkEnabled->IsChecked->Value) :
		ReadBool("net.enable", true);
	int networkBackendValue = saveNetwork ?
		(vlanEnabled->IsChecked->Value ? 3 : networkBackend->SelectedIndex) :
		ReadInt("net.backend", 0);
	String^ udpBindAddressValue = saveNetwork ? udpBindAddress->Text :
		ReadString("net.udp.bind_addr", "0.0.0.0:9368");
	int udpServerValue = saveNetwork ? ClampIndex(udpServer->SelectedIndex, 4, 0) :
		ClampIndex(ReadInt("net.udp.server", 0), 4, 0);
	static const wchar_t *udpServers[] = {
		L"", L"us-west-1.lan.xemu.app:9938",
		L"us-east-1.lan.xemu.app:9938", L"de-1.lan.xemu.app:9938"
	};
	String^ udpRemoteAddressValue = udpServerValue == 0 ?
		(saveNetwork ? udpRemoteAddress->Text :
		 ReadString("net.udp.remote_addr", "")) :
		ref new String(udpServers[udpServerValue]);
	String^ natForwardPortsValue = saveNetwork ? natForwardPorts->Text :
		ReadString("net.nat.forward_ports", "");
#define SAVE_BOOL(key, control) values->Insert(key, control->IsChecked->Value)
#define SAVE_INT(key, value) values->Insert(key, static_cast<int>(value))
#define SAVE_DOUBLE(key, value) values->Insert(key, static_cast<double>(value))
	SAVE_BOOL("general.skip_boot_anim", skipBootAnimation);
	SAVE_BOOL("perf.hard_fpu", hardFpu);
	SAVE_BOOL("perf.cache_shaders", cacheShaders);
	SAVE_BOOL("general.snapshots.filter_current_game", filterSnapshots);
	SAVE_BOOL("input.auto_bind", autoBind);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_left_x", invertLeftX);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_left_y", invertLeftY);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_right_x", invertRightX);
	SAVE_BOOL("input.uwp_gamepad.invert_axis_right_y", invertRightY);
	SAVE_INT("input.port1.driver", port1Driver->SelectedIndex);
	SAVE_INT("input.port2.driver", port2Driver->SelectedIndex);
	SAVE_INT("input.port3.driver", port3Driver->SelectedIndex);
	SAVE_INT("input.port4.driver", port4Driver->SelectedIndex);
	SAVE_INT("input.port1.slot_a", port1SlotA->SelectedIndex);
	SAVE_INT("input.port1.slot_b", port1SlotB->SelectedIndex);
	SAVE_INT("input.port2.slot_a", port2SlotA->SelectedIndex);
	SAVE_INT("input.port2.slot_b", port2SlotB->SelectedIndex);
	SAVE_INT("input.port3.slot_a", port3SlotA->SelectedIndex);
	SAVE_INT("input.port3.slot_b", port3SlotB->SelectedIndex);
	SAVE_INT("input.port4.slot_a", port4SlotA->SelectedIndex);
	SAVE_INT("input.port4.slot_b", port4SlotB->SelectedIndex);
	SAVE_INT("display.quality.surface_scale", surfaceScale->SelectedIndex + 1);
	SAVE_INT("display.filtering", filtering->SelectedIndex);
	SAVE_INT("display.ui.fit", displayFit->SelectedIndex);
	SAVE_INT("display.ui.aspect_ratio", aspectRatio->SelectedIndex);
	SAVE_BOOL("display.window.vsync", vsync);
	SAVE_BOOL("display.ui.show_notifications", showNotifications);
	SAVE_BOOL("display.ui.use_animations", useAnimations);
	SAVE_BOOL("display.ui.auto_scale", autoUiScale);
	SAVE_DOUBLE("display.ui.scale", uiScale->Value);
	SAVE_BOOL("audio.use_dsp", useDsp);
	SAVE_BOOL("audio.hrtf", useHrtf);
	SAVE_DOUBLE("audio.volume_limit", volumeLimit->Value);
	SAVE_INT("sys.mem_limit", memoryLimit->SelectedIndex);
	SAVE_INT("sys.avpack", avPack->SelectedIndex);
#undef SAVE_BOOL
#undef SAVE_INT
#undef SAVE_DOUBLE

	static const char *filterValues[] = { "linear", "nearest" };
	static const char *fitValues[] = { "center", "scale", "stretch" };
	static const char *aspectValues[] = { "native", "auto", "4x3", "16x9" };
	static const char *backendValues[] = { "nat", "udp", "pcap", "vlan" };
	static const char *avValues[] = { "scart", "hdtv", "vga", "rfu", "svideo", "composite", "none" };
	static const char *controllerDrivers[] = { "usb-xbox-gamepad", "usb-xbox-gamepad-s" };
	auto futureFiles = StorageApplicationPermissions::FutureAccessList;
	auto validateXmu = [this, futureFiles](ComboBox^ slot, String^ tag) {
		if (slot->SelectedIndex == 1 && !futureFiles->ContainsItem("xemu-" + tag)) {
				errorText->Text = "Select the Memory Unit file " + tag +
				                  " on the Storage page.";
			return false;
		}
		return true;
	};
	if (!validateXmu(port1SlotA, "xmu-p1a") ||
	    !validateXmu(port1SlotB, "xmu-p1b") ||
	    !validateXmu(port2SlotA, "xmu-p2a") ||
	    !validateXmu(port2SlotB, "xmu-p2b") ||
	    !validateXmu(port3SlotA, "xmu-p3a") ||
	    !validateXmu(port3SlotB, "xmu-p3b") ||
	    !validateXmu(port4SlotA, "xmu-p4a") ||
	    !validateXmu(port4SlotB, "xmu-p4b")) {
		return false;
	}

	std::ostringstream natRules;
	std::istringstream rules(Utf8(natForwardPortsValue));
	std::string rule;
	while (std::getline(rules, rule)) {
		if (rule.empty()) continue;
		std::istringstream fields(rule);
		std::string protocol, host, guest, extra;
		if (!std::getline(fields, protocol, ',') ||
		    !std::getline(fields, host, ',') ||
		    !std::getline(fields, guest, ',') || std::getline(fields, extra, ',')) {
			errorText->Text = "Invalid NAT rule. Use: tcp|udp,host port,Xbox port.";
			return false;
		}
		int hostPort = atoi(host.c_str());
		int guestPort = atoi(guest.c_str());
		if ((protocol != "tcp" && protocol != "udp") || hostPort < 1 ||
		    hostPort > 65535 || guestPort < 1 || guestPort > 65535) {
			errorText->Text = "Invalid NAT rule. Use ports from 1 to 65535.";
			return false;
		}
		natRules << "[[net.nat.forward_ports]]\nhost = " << hostPort
		         << "\nguest = " << guestPort << "\nprotocol = \""
		         << protocol << "\"\n";
	}
	if (saveNetwork) {
		values->Insert("net.enable", networkEnabledValue);
		values->Insert("net.backend", networkBackendValue);
		values->Insert("net.udp.bind_addr", udpBindAddressValue);
		values->Insert("net.udp.server", udpServerValue);
		values->Insert("net.udp.remote_addr", udpRemoteAddressValue);
		values->Insert("net.nat.forward_ports", natForwardPortsValue);
		values->Insert("net.vlan.enabled", vlanEnabled->IsChecked->Value);
		values->Insert("net.vlan.role", vlanRole->SelectedIndex);
		values->Insert("net.vlan.coordinator", vlanCoordinator->Text);
		values->Insert("net.vlan.room_code", vlanRoomCode->Text);
	}

	std::ostringstream config;
	config << "[general]\n"
	       << "skip_boot_anim = " << BoolText(skipBootAnimation->IsChecked->Value) << "\n"
	       << "screenshot_dir = " << (StorageApplicationPermissions::FutureAccessList->ContainsItem("xemu-screenshots") ? "\"/broker/screenshots\"" : "\"\"") << "\n"
	       << "games_dir = \"/broker/games\"\n"
	       << "[general.updates]\ncheck = false\n"
	       << "[general.snapshots]\nfilter_current_game = " << BoolText(filterSnapshots->IsChecked->Value) << "\n"
	       << "[perf]\nhard_fpu = " << BoolText(hardFpu->IsChecked->Value) << "\ncache_shaders = " << BoolText(cacheShaders->IsChecked->Value) << "\n"
	       << "[input]\nauto_bind = " << BoolText(autoBind->IsChecked->Value) << "\nbackground_input_capture = false\n"
	       << "[input.bindings]\nport1_driver = \"" << controllerDrivers[port1Driver->SelectedIndex]
	       << "\"\nport2_driver = \"" << controllerDrivers[port2Driver->SelectedIndex]
	       << "\"\nport3_driver = \"" << controllerDrivers[port3Driver->SelectedIndex]
	       << "\"\nport4_driver = \"" << controllerDrivers[port4Driver->SelectedIndex] << "\"\n"
	       << "[input.uwp_gamepad]\nenable_rumble = false\ninvert_axis_left_x = " << BoolText(invertLeftX->IsChecked->Value)
	       << "\ninvert_axis_left_y = " << BoolText(invertLeftY->IsChecked->Value)
	       << "\ninvert_axis_right_x = " << BoolText(invertRightX->IsChecked->Value)
	       << "\ninvert_axis_right_y = " << BoolText(invertRightY->IsChecked->Value) << "\n"
	       << "[input.peripherals.port1]\nperipheral_type_0 = " << port1SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p1a\"\nperipheral_type_1 = " << port1SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p1b\"\n"
	       << "[input.peripherals.port2]\nperipheral_type_0 = " << port2SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p2a\"\nperipheral_type_1 = " << port2SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p2b\"\n"
	       << "[input.peripherals.port3]\nperipheral_type_0 = " << port3SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p3a\"\nperipheral_type_1 = " << port3SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p3b\"\n"
	       << "[input.peripherals.port4]\nperipheral_type_0 = " << port4SlotA->SelectedIndex << "\nperipheral_param_0 = \"/broker/xmu-p4a\"\nperipheral_type_1 = " << port4SlotB->SelectedIndex << "\nperipheral_param_1 = \"/broker/xmu-p4b\"\n"
	       << "[display]\nrenderer = \"OPENGL\"\nfiltering = \"" << filterValues[filtering->SelectedIndex] << "\"\n"
	       << "[display.quality]\nsurface_scale = " << surfaceScale->SelectedIndex + 1 << "\n"
	       << "[display.window]\nfullscreen_on_startup = false"
	       << "\nfullscreen_exclusive = false"
	       << "\nstartup_size = \"1280x960\"\nvsync = " << BoolText(vsync->IsChecked->Value) << "\n"
	       << "[display.ui]\nshow_menubar = false"
	       << "\nshow_notifications = " << BoolText(showNotifications->IsChecked->Value)
	       << "\nhide_cursor = true"
	       << "\nuse_animations = " << BoolText(useAnimations->IsChecked->Value)
	       << "\nfit = \"" << fitValues[displayFit->SelectedIndex] << "\"\naspect_ratio = \"" << aspectValues[aspectRatio->SelectedIndex]
	       << "\"\nscale = " << uiScale->Value << "\nauto_scale = " << BoolText(autoUiScale->IsChecked->Value) << "\n"
	       << "[audio]\nuse_dsp = " << BoolText(useDsp->IsChecked->Value)
	       << "\nuse_dsp_jit = false"
	       << "\nhrtf = " << BoolText(useHrtf->IsChecked->Value) << "\nvolume_limit = " << volumeLimit->Value << "\n"
	       << "[audio.vp]\nnum_workers = 1\n"
	       << "[net]\nenable = " << BoolText(networkEnabledValue) << "\nbackend = \"" << backendValues[networkBackendValue] << "\"\n"
	       << "[net.udp]\nbind_addr = " << TomlString(udpBindAddressValue) << "\nremote_addr = " << TomlString(udpRemoteAddressValue) << "\n"
	       << "[net.vlan]\nhost = " << BoolText(vlanRole->SelectedIndex == 0)
	       << "\nbind_addr = \"127.0.0.1:9941\"\nremote_addr = \"127.0.0.1:9940\"\n"
	       << natRules.str()
	       << "[sys]\nmem_limit = \"" << (memoryLimit->SelectedIndex == 0 ? "64" : "128")
	       << "\"\navpack = \"" << avValues[avPack->SelectedIndex] << "\"\n";

	auto path = ApplicationData::Current->LocalFolder->Path + "\\xemu.toml";
	CREATEFILE2_EXTENDED_PARAMETERS parameters = {};
	parameters.dwSize = sizeof(parameters);
	parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	HANDLE file = CreateFile2(path->Data(), GENERIC_WRITE, FILE_SHARE_READ,
	                          CREATE_ALWAYS, &parameters);
	if (file == INVALID_HANDLE_VALUE) {
		errorText->Text = "Failed to open xemu.toml for writing.";
		return false;
	}
	std::string text = config.str();
	DWORD written = 0;
	bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
	                    &written, nullptr) && written == text.size();
	CloseHandle(file);
	if (!ok) {
		errorText->Text = "Failed to save all settings to xemu.toml.";
	}
	return ok;
}

void DirectXPage::SelectFile_Click(Object^ sender, RoutedEventArgs^)
{
	auto button = safe_cast<Button^>(sender);
	auto picker = ref new Windows::Storage::Pickers::FileOpenPicker();
	picker->FileTypeFilter->Append("*");
	create_task(picker->PickSingleFileAsync()).then([this, button](Windows::Storage::StorageFile^ file) {
		if (!file) return;
		auto tagValue = button->Tag->ToString();
	MountXboxFile(file, tagValue, tagValue != "dvd");
	});
}

void DirectXPage::SelectFolder_Click(Object^ sender, RoutedEventArgs^)
{
	auto button = safe_cast<Button^>(sender);
	auto picker = ref new Windows::Storage::Pickers::FolderPicker();
	picker->FileTypeFilter->Append("*");
	create_task(picker->PickSingleFolderAsync()).then(
		[this, button](StorageFolder^ folder) {
			if (folder) MountXboxFolder(folder, button->Tag->ToString(), true);
		});
}

void DirectXPage::MountXboxFolder(StorageFolder^ folder, String^ tagValue,
	                               bool persist)
{
	auto status = tagValue == "screenshots" ? screenshotFolderStatus :
	                                           gamesFolderStatus;
	status->Text = folder->Path->IsEmpty() ? folder->Name : folder->Path;
	if (persist) {
		StorageApplicationPermissions::FutureAccessList->AddOrReplace(
			"xemu-" + tagValue, folder);
	}
	std::string tag = tagValue == "screenshots" ? "screenshots" : "games";
	if (!m_xemu->MountFolder("/broker/" + tag, folder)) {
		errorText->Text = "Failed to mount the selected folder.";
		toolTabs->SelectedIndex = 6;
	}
}

void DirectXPage::MountXboxFile(StorageFile^ file, String^ tagValue,
	                            bool persist)
{
	bool *mountPending = tagValue == "flash" ? &m_flashMountPending :
	                     tagValue == "bootrom" ? &m_bootromMountPending :
	                     tagValue == "hdd" ? &m_hddMountPending : nullptr;
	if (mountPending && *mountPending) {
		return;
	}
	if (mountPending) {
		*mountPending = true;
	}
	auto status = tagValue == "flash" ? flashFileStatus :
	              tagValue == "bootrom" ? bootromFileStatus :
	              tagValue == "hdd" ? hddFileStatus :
	              tagValue == "eeprom" ? eepromFileStatus :
	              tagValue == "xmu-p1a" ? xmuP1AStatus :
	              tagValue == "xmu-p1b" ? xmuP1BStatus :
	              tagValue == "xmu-p2a" ? xmuP2AStatus :
	              tagValue == "xmu-p2b" ? xmuP2BStatus :
	              tagValue == "xmu-p3a" ? xmuP3AStatus :
	              tagValue == "xmu-p3b" ? xmuP3BStatus :
	              tagValue == "xmu-p4a" ? xmuP4AStatus :
	              tagValue == "xmu-p4b" ? xmuP4BStatus : dvdFileStatus;
	auto location = file->Path->IsEmpty() ? file->Name : file->Path;
	status->Text = location + "  |  " + file->Name;
	if (persist) {
		StorageApplicationPermissions::FutureAccessList->AddOrReplace(
			"xemu-" + tagValue, file);
	}

	bool isXmu = tagValue->Length() >= 4 &&
	             wcsncmp(tagValue->Data(), L"xmu-", 4) == 0;
	if (tagValue == "xmu-p1a") port1SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p1b") port1SlotB->SelectedIndex = 1;
	else if (tagValue == "xmu-p2a") port2SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p2b") port2SlotB->SelectedIndex = 1;
	else if (tagValue == "xmu-p3a") port3SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p3b") port3SlotB->SelectedIndex = 1;
	else if (tagValue == "xmu-p4a") port4SlotA->SelectedIndex = 1;
	else if (tagValue == "xmu-p4b") port4SlotB->SelectedIndex = 1;

	if (persist && isXmu) {
		SaveSettings(false);
	}
	auto access = (tagValue == "hdd" || tagValue == "eeprom" || isXmu) ? FileAccessMode::ReadWrite :
	                                  FileAccessMode::Read;
	create_task(file->OpenAsync(access)).then(
		[this, file, tagValue](Windows::Storage::Streams::IRandomAccessStream^ stream) {
			std::string tag = Utf8(tagValue);
			if (!m_xemu->MountFile("/broker/" + tag, file, stream)) {
				if (tagValue == "flash") { m_flashReady = false; m_flashMountPending = false; }
				else if (tagValue == "bootrom") { m_bootromReady = false; m_bootromMountPending = false; }
				else if (tagValue == "hdd") { m_hddReady = false; m_hddMountPending = false; }
				else if (tagValue == "dvd") m_dvdReady = false;
				UpdateStartButtonState();
				auto error = m_xemu->LastError();
				errorText->Text = ref new String(
					std::wstring(error.begin(), error.end()).c_str());
				toolTabs->SelectedIndex = 6;
				return;
			}
			if (tagValue == "flash") { m_flashReady = true; m_flashMountPending = false; }
			else if (tagValue == "bootrom") { m_bootromReady = true; m_bootromMountPending = false; }
			else if (tagValue == "hdd") { m_hddReady = true; m_hddMountPending = false; }
			else if (tagValue == "dvd") m_dvdReady = true;
			UpdateStartButtonState();
		}).then([this, file, tagValue](task<void> result) {
			try {
				result.get();
			} catch (Platform::Exception^ exception) {
				if (tagValue == "flash") { m_flashReady = false; m_flashMountPending = false; }
				else if (tagValue == "bootrom") { m_bootromReady = false; m_bootromMountPending = false; }
				else if (tagValue == "hdd") { m_hddReady = false; m_hddMountPending = false; }
				else if (tagValue == "dvd") m_dvdReady = false;
				UpdateStartButtonState();
				errorText->Text = "Failed to open " + file->Name + ": " +
				                  exception->Message;
				toolTabs->SelectedIndex = 6;
			}
		});
}

void DirectXPage::RestorePersistedFiles()
{
	PrepareLocalMachineFolder("BIOS", "flash");
	PrepareLocalMachineFolder("MCPX", "bootrom");
	PrepareLocalMachineFolder("hard_disk", "hdd");
	if (StorageApplicationPermissions::FutureAccessList->ContainsItem(
		    "xemu-flash")) RestorePersistedFile("flash");
	if (StorageApplicationPermissions::FutureAccessList->ContainsItem(
		    "xemu-bootrom")) RestorePersistedFile("bootrom");
	if (StorageApplicationPermissions::FutureAccessList->ContainsItem(
		    "xemu-hdd")) RestorePersistedFile("hdd");
	RestorePersistedFile("eeprom");
	RestorePersistedFile("xmu-p1a");
	RestorePersistedFile("xmu-p1b");
	RestorePersistedFile("xmu-p2a");
	RestorePersistedFile("xmu-p2b");
	RestorePersistedFile("xmu-p3a");
	RestorePersistedFile("xmu-p3b");
	RestorePersistedFile("xmu-p4a");
	RestorePersistedFile("xmu-p4b");
	RestorePersistedFolder("screenshots");
	if (StorageApplicationPermissions::FutureAccessList->ContainsItem(
		    "xemu-games")) {
		RestorePersistedFolder("games");
	} else {
		MountDefaultGamesFolder();
	}
}

void DirectXPage::PrepareLocalMachineFolder(String^ folderName,
	                                         String^ tagValue)
{
	if ((tagValue == "flash" && (m_flashReady || m_flashMountPending)) ||
	    (tagValue == "bootrom" && (m_bootromReady || m_bootromMountPending)) ||
	    (tagValue == "hdd" && (m_hddReady || m_hddMountPending))) {
		return;
	}
	create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
		folderName, CreationCollisionOption::OpenIfExists))
		.then([this, folderName, tagValue](StorageFolder^ folder) {
			auto token = "xemu-" + tagValue;
			if (StorageApplicationPermissions::FutureAccessList->ContainsItem(token)) {
				return task_from_result();
			}
			auto status = tagValue == "flash" ? flashFileStatus :
			              tagValue == "bootrom" ? bootromFileStatus : hddFileStatus;
			status->Text = folder->Path + "  |  No file found";
			return create_task(folder->GetFilesAsync()).then(
				[this, tagValue](Windows::Foundation::Collections::IVectorView<StorageFile^>^ files) {
					if (files->Size > 0 &&
					    !StorageApplicationPermissions::FutureAccessList->ContainsItem(
						    "xemu-" + tagValue)) {
						MountXboxFile(files->GetAt(0), tagValue, false);
					}
				});
		}).then([this, folderName](task<void> result) {
			try {
				result.get();
			} catch (Platform::Exception^ exception) {
				errorText->Text = "Failed to prepare LocalState\\" + folderName +
				                  ": " + exception->Message;
			}
		});
}

void DirectXPage::MountDefaultGamesFolder()
{
	create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
		"games", CreationCollisionOption::OpenIfExists))
		.then([this](StorageFolder^ folder) {
			/* A user selection may complete while LocalState is being opened.
			   The Future Access List always wins over the default folder. */
			if (!StorageApplicationPermissions::FutureAccessList->ContainsItem(
				    "xemu-games")) {
				MountXboxFolder(folder, "games", false);
			}
		}).then([this](task<void> result) {
			try {
				result.get();
			} catch (Platform::Exception^ exception) {
				errorText->Text = "Failed to create LocalState\\games: " +
				                  exception->Message;
			}
		});
}

void DirectXPage::RestorePersistedFile(String^ tagValue)
{
	auto token = "xemu-" + tagValue;
	if (!StorageApplicationPermissions::FutureAccessList->ContainsItem(token)) {
		return;
	}
	create_task(StorageApplicationPermissions::FutureAccessList->GetFileAsync(token))
		.then([this, tagValue](StorageFile^ file) {
			MountXboxFile(file, tagValue, false);
		}).then([this, tagValue, token](task<void> result) {
			try {
				result.get();
			}
			catch (Platform::Exception^ exception)
			{
				errorText->Text = "Failed to restore saved file: " +
				                  exception->Message;
				StorageApplicationPermissions::FutureAccessList->Remove(token);
				if (tagValue == "flash") PrepareLocalMachineFolder("BIOS", tagValue);
				else if (tagValue == "bootrom") PrepareLocalMachineFolder("MCPX", tagValue);
				else if (tagValue == "hdd") PrepareLocalMachineFolder("hard_disk", tagValue);
			}
		});
}

void DirectXPage::RestorePersistedFolder(String^ tagValue)
{
	auto token = "xemu-" + tagValue;
	if (!StorageApplicationPermissions::FutureAccessList->ContainsItem(token)) {
		return;
	}
	create_task(StorageApplicationPermissions::FutureAccessList->GetFolderAsync(token))
		.then([this, tagValue](StorageFolder^ folder) {
			MountXboxFolder(folder, tagValue, false);
		}).then([this, tagValue, token](task<void> result) {
			try { result.get(); }
			catch (Platform::Exception^ exception) {
				errorText->Text = "Failed to restore saved folder: " + exception->Message;
				if (tagValue == "games") {
					StorageApplicationPermissions::FutureAccessList->Remove(token);
					MountDefaultGamesFolder();
				}
			}
		});
}
