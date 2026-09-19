//
// App.xaml.cpp
// Implementação da classe App.
//

#include "pch.h"
#include "DirectXPage.xaml.h"

using namespace UWP_Port;

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::System::Profile;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Interop;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::ViewManagement;

namespace
{
bool IsXboxDevice()
{
	try
	{
		auto versionInfo = AnalyticsInfo::VersionInfo;
		return versionInfo != nullptr &&
			versionInfo->DeviceFamily == "Windows.Xbox";
	}
	catch (Platform::Exception^)
	{
		return false;
	}
}
}
/// <summary>
/// Inicializa o objeto singleton do aplicativo.  Esta é a primeira linha de código criado
/// executado e, como tal, é o equivalente lógico de main() ou WinMain().
/// </summary>
App::App()
{
	InitializeComponent();
	Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
	Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);

	// The OS enforces a hard per-app memory quota in the UWP/Xbox sandbox
	// (unlike desktop Win32, going over it leads to suspension or
	// termination rather than paging). React early so xemu's buffers get
	// trimmed before that happens.
	Windows::System::MemoryManager::AppMemoryUsageIncreased +=
		ref new EventHandler<Object^>(this, &App::OnAppMemoryUsageIncreased);
	Windows::System::MemoryManager::AppMemoryUsageLimitChanging +=
		ref new EventHandler<Windows::System::AppMemoryUsageLimitChangingEventArgs^>(
			this, &App::OnAppMemoryUsageLimitChanging);
}

/// <summary>
/// Chamado quando o aplicativo é iniciado normalmente pelo usuário final.  Outros pontos de entrada
/// serão usados quando o aplicativo é iniciado para abrir um arquivo específico, para exibir
/// resultados da pesquisa e assim por diante.
/// </summary>
/// <param name="e">Detalhes sobre a solicitação e o processo de inicialização.</param>
void App::OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e)
{
#if _DEBUG
	if (IsDebuggerPresent())
	{
		DebugSettings->EnableFrameRateCounter = true;
	}
#endif
	// This must happen before creating the XAML visual tree. Otherwise Xbox
	// automatically enlarges the whole interface for ten-foot presentation.
	ConfigureWindowBounds();

	auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);

	// Não repita a inicialização do aplicativo quando a Janela já tiver conteúdo,
	// apenas verifique se a janela está ativa
	if (rootFrame == nullptr)
	{
		// Criar um Quadro para agir como o contexto de navegação e associá-lo a
		// uma chave SuspensionManager
		rootFrame = ref new Frame();

		rootFrame->NavigationFailed += ref new Windows::UI::Xaml::Navigation::NavigationFailedEventHandler(this, &App::OnNavigationFailed);

		// Coloque o quadro na Janela atual
		Window::Current->Content = rootFrame;
	}

	if (rootFrame->Content == nullptr)
	{
		// Quando a pilha de navegação não for restaurada, navegar para a primeira página,
		// configurando a nova página passando as informações necessárias como um parâmetro
		// parâmetro
		try
		{
			rootFrame->Navigate(TypeName(DirectXPage::typeid), e->Arguments);
		}
		catch (Platform::Exception^ ex)
		{
			auto path = ApplicationData::Current->LocalFolder->Path + "\\launch-error.txt";
			CREATEFILE2_EXTENDED_PARAMETERS params{};
			params.dwSize = sizeof(params);
			params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
			HANDLE file = CreateFile2(path->Data(), GENERIC_WRITE, FILE_SHARE_READ,
			                          CREATE_ALWAYS, &params);
			if (file != INVALID_HANDLE_VALUE)
			{
				DWORD written;
				WriteFile(file, ex->Message->Data(), ex->Message->Length() * sizeof(wchar_t), &written, nullptr);
				CloseHandle(file);
			}
			auto message = ref new TextBlock();
			message->Text = "Failed to load the interface: " + ex->Message;
			message->TextWrapping = TextWrapping::Wrap;
			message->Margin = Thickness(32);
			rootFrame->Content = message;
		}
	}

	if (m_directXPage == nullptr)
	{
		m_directXPage = dynamic_cast<DirectXPage^>(rootFrame->Content);
	}

	if (e->PreviousExecutionState == ApplicationExecutionState::Terminated)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}
	
	// Verifique se a janela atual está ativa
	Window::Current->Activate();
}

void App::ConfigureWindowBounds()
{
	try
	{
		if (!IsXboxDevice())
		{
			return;
		}

		ApplicationViewScaling::TrySetDisableLayoutScaling(true);
		auto view = ApplicationView::GetForCurrentView();
		view->SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
	}
	catch (Platform::Exception^)
	{
		// Retain the system-selected bounds if this API is unavailable.
	}
}
/// <summary>
/// Chamado quando a execução do aplicativo está sendo suspensa.  O estado do aplicativo é salvo
/// sem saber se o aplicativo será encerrado ou retomado com o conteúdo
/// da memória ainda intacto.
/// </summary>
/// <param name="sender">A fonte da solicitação de suspensão.</param>
/// <param name="e">Detalhes sobre a solicitação de suspensão.</param>
void App::OnSuspending(Object^ sender, SuspendingEventArgs^ e)
{
	(void) sender;	// Parâmetro não usado
	(void) e;	// Parâmetro não usado

	m_directXPage->SaveInternalState(ApplicationData::Current->LocalSettings->Values);

	// SaveInternalState already paused xemu, so the CPU/GPU emulation loop
	// is idle at this point. Hand back whatever working set it built up
	// (shader cache, translated-code cache, framebuffers) while suspended
	// so the OS can give that memory to other apps instead of counting it
	// against this app's quota the whole time it's backgrounded.
	TrimWorkingSet();
}

/// <summary>
/// Fired as overall system-wide app memory usage rises. Used here purely as
/// an early warning to release reclaimable memory before the app's own
/// usage limit is hit.
/// </summary>
void App::OnAppMemoryUsageIncreased(Object^ sender, Object^ args)
{
	(void) sender;
	(void) args;

	auto level = Windows::System::MemoryManager::AppMemoryUsageLevel;
	if (level == Windows::System::AppMemoryUsageLevel::High ||
	    level == Windows::System::AppMemoryUsageLevel::OverLimit)
	{
		TrimWorkingSet();
	}
}

/// <summary>
/// Fired when the OS is about to change (typically lower) this app's memory
/// quota, e.g. another app or game is being launched alongside it. Trim
/// proactively so the app doesn't get suspended for exceeding the new
/// limit a moment later.
/// </summary>
void App::OnAppMemoryUsageLimitChanging(Object^ sender,
	Windows::System::AppMemoryUsageLimitChangingEventArgs^ args)
{
	(void) sender;

	auto currentUsage = Windows::System::MemoryManager::AppMemoryUsage;
	if (args != nullptr && currentUsage >= args->NewLimit)
	{
		TrimWorkingSet();
	}
}

/// <summary>
/// Releases currently-unused pages in this process's working set back to
/// the OS. This does not free any live allocation or emulator state; it
/// only asks Windows to page out memory the process isn't actively
/// touching, which Windows will happily page back in on demand. Safe to
/// call at any time, including while xemu is running.
/// </summary>
void App::TrimWorkingSet()
{
	SetProcessWorkingSetSize(GetCurrentProcess(),
	                          static_cast<SIZE_T>(-1),
	                          static_cast<SIZE_T>(-1));
}

/// <summary>
/// Invocado quando a execução do aplicativo é retomada.
/// </summary>
/// <param name="sender">A origem da solicitação de retomada.</param>
/// <param name="args">Detalhes sobre a solicitação de retomada.</param>
void App::OnResuming(Object ^sender, Object ^args)
{
	(void) sender; // Parâmetro não usado
	(void) args; // Parâmetro não usado

	m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
}

/// <summary>
/// Chamado quando ocorre uma falha na Navegação para uma determinada página
/// </summary>
/// <param name="sender">O Quadro com navegação com falha</param>
/// <param name="e">Detalhes sobre a falha na navegação</param>
void App::OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e)
{
	throw ref new FailureException("Failed to load Page " + e->SourcePageType.Name);
}

