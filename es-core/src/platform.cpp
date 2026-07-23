#include "platform.h"

#include <SDL_events.h>
#ifdef WIN32
#include <codecvt>
#include <Windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#endif
#include <fcntl.h>
#include <vector>

#include "Log.h"
#include "utils/FileSystemUtil.h"

int runShutdownCommand()
{
#ifdef WIN32 // windows
	return system("shutdown -s -t 0");
#else // osx / linux
	return system("sudo shutdown -h now");
#endif
}

int runRestartCommand()
{
#ifdef WIN32 // windows
	return system("shutdown -r -t 0");
#else // osx / linux
	return system("sudo shutdown -r now");
#endif
}

int runSystemCommand(const std::string& cmd_utf8)
{
#ifdef WIN32
	// on Windows we use _wsystem to support non-ASCII paths
	// which requires converting from utf8 to a wstring
	typedef std::codecvt_utf8<wchar_t> convert_type;
	std::wstring_convert<convert_type, wchar_t> converter;
	std::wstring wchar_str = converter.from_bytes(cmd_utf8);
	return _wsystem(wchar_str.c_str());
#else
	return system(cmd_utf8.c_str());
#endif
}

QuitMode quitMode = QuitMode::QUIT;

int quitES(QuitMode mode)
{
	quitMode = mode;

	SDL_Event *quit = new SDL_Event();
	quit->type = SDL_QUIT;
	SDL_PushEvent(quit);
	return 0;
}

void touch(const std::string& filename)
{
#ifdef WIN32
	FILE* fp = fopen(filename.c_str(), "ab+");
	if (fp != NULL)
		fclose(fp);
#else
	int fd = open(filename.c_str(), O_CREAT|O_WRONLY, 0644);
	if (fd >= 0)
		close(fd);
#endif
}

void processQuitMode()
{
	switch (quitMode)
	{
	case QuitMode::RESTART:
		LOG(LogInfo) << "Restarting EmulationStation";
		touch("/tmp/es-restart");
		break;
	case QuitMode::REBOOT:
		LOG(LogInfo) << "Rebooting system";
		touch("/tmp/es-sysrestart");
		runRestartCommand();
		break;
	case QuitMode::SHUTDOWN:
		LOG(LogInfo) << "Shutting system down";
		touch("/tmp/es-shutdown");
		runShutdownCommand();
		break;
	default:
		// No-op to prevent compiler warnings
		// If we reach here, it is not a RESTART, REBOOT,
		// or SHUTDOWN. Basically a normal exit.
		break;
	}
}

#ifdef WIN32
static HANDLE gSearchServiceHandle = nullptr;

void startSearchService()
{
	std::string exePath = Utils::FileSystem::getExePath() + "/es-search-service.exe";
	if (!Utils::FileSystem::exists(exePath))
	{
		LOG(LogWarning) << "es-search-service.exe not found in " << Utils::FileSystem::getExePath();
		return;
	}

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	DWORD creationFlags = CREATE_NO_WINDOW;

	std::string cmd = "\"" + exePath + "\"";
	std::vector<char> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back('\0');

	if (CreateProcessA(
		nullptr,
		cmdBuf.data(),
		nullptr,
		nullptr,
		FALSE,
		creationFlags,
		nullptr,
		Utils::FileSystem::getExePath().c_str(),
		&si,
		&pi
	))
	{
		gSearchServiceHandle = pi.hProcess;
		CloseHandle(pi.hThread);
		LOG(LogInfo) << "Started search service process (PID: " << pi.dwProcessId << ")";
	}
	else
	{
		LOG(LogError) << "Failed to start search service process! Error code: " << GetLastError();
	}
}

void stopSearchService()
{
	if (gSearchServiceHandle != nullptr)
	{
		LOG(LogInfo) << "Terminating search service process...";
		TerminateProcess(gSearchServiceHandle, 0);
		CloseHandle(gSearchServiceHandle);
		gSearchServiceHandle = nullptr;
	}
}
#else
static pid_t gSearchServicePid = -1;

void startSearchService()
{
	std::string exePath = Utils::FileSystem::getExePath() + "/es-search-service";
	if (!Utils::FileSystem::exists(exePath))
	{
		LOG(LogWarning) << "es-search-service not found in " << Utils::FileSystem::getExePath();
		return;
	}

	pid_t pid = fork();
	if (pid == -1)
	{
		LOG(LogError) << "Failed to fork search service process!";
	}
	else if (pid == 0)
	{
		int devNull = open("/dev/null", O_RDWR);
		if (devNull >= 0)
		{
			dup2(devNull, STDIN_FILENO);
			dup2(devNull, STDOUT_FILENO);
			dup2(devNull, STDERR_FILENO);
			close(devNull);
		}

		char* const argv[] = { const_cast<char*>(exePath.c_str()), nullptr };
		execv(exePath.c_str(), argv);

		exit(1);
	}
	else
	{
		gSearchServicePid = pid;
		LOG(LogInfo) << "Started search service process (PID: " << pid << ")";
	}
}

void stopSearchService()
{
	if (gSearchServicePid > 0)
	{
		LOG(LogInfo) << "Terminating search service process (PID: " << gSearchServicePid << ")...";
		kill(gSearchServicePid, SIGTERM);
		
		int status;
		int attempts = 0;
		while (waitpid(gSearchServicePid, &status, WNOHANG) == 0 && attempts < 10)
		{
			usleep(100000);
			attempts++;
		}
		
		if (waitpid(gSearchServicePid, &status, WNOHANG) == 0)
		{
			LOG(LogWarning) << "Search service process did not terminate cleanly. Force killing (SIGKILL)...";
			kill(gSearchServicePid, SIGKILL);
			waitpid(gSearchServicePid, &status, 0);
		}
		
		gSearchServicePid = -1;
	}
}
#endif
