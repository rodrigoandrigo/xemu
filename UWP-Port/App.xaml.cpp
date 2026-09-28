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
using namespace Windows::System;
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
	m_memoryIncreasedToken = MemoryManager::AppMemoryUsageIncreased +=
		ref new EventHandler<Object^>(this, &App::OnMemoryUsageChanged);
	m_memoryDecreasedToken = MemoryManager::AppMemoryUsageDecreased +=
		ref new EventHandler<Object^>(this, &App::OnMemoryUsageChanged);
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
	EnsureMainPage(e->Arguments);

	if (e->PreviousExecutionState == ApplicationExecutionState::Terminated)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}
	Window::Current->Activate();
}

void App::EnsureMainPage(Object^ parameter)
{
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
			rootFrame->Navigate(TypeName(DirectXPage::typeid), parameter);
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

}

void App::OnActivated(IActivatedEventArgs^ e)
{
	if (e->Kind != ActivationKind::Protocol) {
		Application::OnActivated(e);
		return;
	}
	auto protocol = safe_cast<ProtocolActivatedEventArgs^>(e);
	EnsureMainPage(protocol->Uri);
	Window::Current->Activate();
	if (m_directXPage != nullptr)
		m_directXPage->HandleProtocolActivation(protocol->Uri);
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
	auto deferral = e->SuspendingOperation->GetDeferral();
	if (m_directXPage != nullptr)
	{
		m_directXPage->SaveInternalState(
			ApplicationData::Current->LocalSettings->Values);
	}
	deferral->Complete();
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

	if (m_directXPage != nullptr)
	{
		m_directXPage->LoadInternalState(
			ApplicationData::Current->LocalSettings->Values);
	}
}

void App::OnMemoryUsageChanged(Object^ sender, Object^ args)
{
	(void)sender;
	(void)args;
	if (m_directXPage == nullptr)
	{
		return;
	}

	QemuHostMemoryPressure pressure = QEMU_HOST_MEMORY_PRESSURE_NORMAL;
	switch (MemoryManager::AppMemoryUsageLevel)
	{
	case AppMemoryUsageLevel::Medium:
		pressure = QEMU_HOST_MEMORY_PRESSURE_MODERATE;
		break;
	case AppMemoryUsageLevel::High:
		pressure = QEMU_HOST_MEMORY_PRESSURE_HIGH;
		break;
	case AppMemoryUsageLevel::OverLimit:
		pressure = QEMU_HOST_MEMORY_PRESSURE_CRITICAL;
		break;
	case AppMemoryUsageLevel::Low:
	default:
		break;
	}
	m_directXPage->HandleMemoryPressure(pressure);
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
