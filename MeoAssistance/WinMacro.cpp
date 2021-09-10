#include "WinMacro.h"

#include <stdint.h>
#include <WinUser.h>

#include <vector>
#include <utility>
#include <algorithm>
#include <chrono>

#include <opencv2/opencv.hpp>

#include "AsstDef.h"
#include "Logger.hpp"
#include "Configer.h"

using namespace asst;

WinMacro::WinMacro(const EmulatorInfo& info)
	: m_emulator_info(info),
	m_rand_engine(std::chrono::system_clock::now().time_since_epoch().count()),
	m_screen_filename(GetCurrentDir() + "adb_screen.png")
{
	findHandle();
}

bool WinMacro::captured() const noexcept
{
	return m_handle != NULL && ::IsWindow(m_handle);
}

bool WinMacro::findHandle()
{
	const HandleInfo& handle_info = m_emulator_info.handle;

	wchar_t* class_wbuff = NULL;
	if (!handle_info.class_name.empty()) {
		size_t class_len = (handle_info.class_name.size() + 1) * 2;
		class_wbuff = new wchar_t[class_len];
		::MultiByteToWideChar(CP_UTF8, 0, handle_info.class_name.c_str(), -1, class_wbuff, class_len);
	}
	wchar_t* window_wbuff = NULL;
	if (!handle_info.window_name.empty()) {
		size_t window_len = (handle_info.window_name.size() + 1) * 2;
		window_wbuff = new wchar_t[window_len];
		memset(window_wbuff, 0, window_len);
		::MultiByteToWideChar(CP_UTF8, 0, handle_info.window_name.c_str(), -1, window_wbuff, window_len);
	}

	m_handle = ::FindWindowExW(m_handle, NULL, class_wbuff, window_wbuff);

	if (class_wbuff != NULL) {
		delete[] class_wbuff;
		class_wbuff = NULL;
	}
	if (window_wbuff != NULL) {
		delete[] window_wbuff;
		window_wbuff = NULL;
	}

	if (m_handle == NULL) {
		return false;
	}
	DWORD pid = 0;
	::GetWindowThreadProcessId(m_handle, &pid);
	HANDLE handle = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	LPSTR path_buff = new CHAR[MAX_PATH];
	memset(path_buff, 0, MAX_PATH);
	DWORD buff_size = MAX_PATH;
	QueryFullProcessImageNameA(handle, 0, path_buff, &buff_size);
	std::string adb_dir = path_buff;
	if (path_buff != NULL) {
		delete[] path_buff;
		path_buff = NULL;
	}
	size_t pos = adb_dir.find_last_of('\\') + 1;
	adb_dir = adb_dir.substr(0, pos);
	adb_dir = '"' + StringReplaceAll(m_emulator_info.adb.path, "[EmulatorPath]", adb_dir) + '"';

	// TODO����������Ƿ�ɹ�
	std::string connect_cmd = StringReplaceAll(m_emulator_info.adb.connect, "[Adb]", adb_dir);
	if (!callCmd(connect_cmd)) {
		DebugTraceError("Connect Adb Error", connect_cmd);
		return false;
	}

	auto&& display_ret = callCmd(StringReplaceAll(m_emulator_info.adb.display, "[Adb]", adb_dir));
	if (display_ret) {
		std::string pipe_str = display_ret.value();
		sscanf_s(pipe_str.c_str(), m_emulator_info.adb.display_regex.c_str(),
			&m_emulator_info.adb.display_width, &m_emulator_info.adb.display_height);
	}
	else {
		DebugTraceError("Get Display Error");
		return false;
	}

	m_emulator_info.adb.click = StringReplaceAll(m_emulator_info.adb.click, "[Adb]", adb_dir);
	m_emulator_info.adb.swipe = StringReplaceAll(m_emulator_info.adb.swipe, "[Adb]", adb_dir);

	m_emulator_info.adb.screencap = StringReplaceAll(
		StringReplaceAll(m_emulator_info.adb.screencap, "[Adb]", adb_dir),
		"[Filename]", m_screen_filename);
	m_emulator_info.adb.pullscreen = StringReplaceAll(
		StringReplaceAll(m_emulator_info.adb.pullscreen, "[Adb]", adb_dir),
		"[Filename]", m_screen_filename);

	DebugTrace("Handle:", m_handle, "Name:", m_emulator_info.name);
	return true;
}

std::optional<std::string> asst::WinMacro::callCmd(const std::string& cmd, bool use_pipe)
{
	// ��ʼ���ܵ�
	constexpr int PipeBuffSize = 1024;
	HANDLE pipe_read = NULL;
	HANDLE pipe_write = NULL;
	SECURITY_ATTRIBUTES sa_out_pipe;
	::ZeroMemory(&sa_out_pipe, sizeof(sa_out_pipe));
	sa_out_pipe.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa_out_pipe.lpSecurityDescriptor = NULL;
	sa_out_pipe.bInheritHandle = TRUE;
	BOOL pipe_ret = FALSE;
	if (use_pipe) {
		pipe_ret = ::CreatePipe(&pipe_read, &pipe_write, &sa_out_pipe, PipeBuffSize);
		DebugTrace("Create Pipe ret", pipe_ret ? "True" : "False");
	}

	// ׼������ADB����
	STARTUPINFOA startup_info;
	PROCESS_INFORMATION process_info;
	ZeroMemory(&startup_info, sizeof(startup_info));
	ZeroMemory(&process_info, sizeof(process_info));
	startup_info.cb = sizeof(STARTUPINFO);
	startup_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	startup_info.hStdOutput = pipe_write;
	startup_info.hStdError = pipe_write;
	startup_info.wShowWindow = SW_HIDE;

	DWORD ret = -1;
	if (!::CreateProcessA(NULL, const_cast<LPSTR>(cmd.c_str()), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup_info, &process_info)) {
		DebugTraceError("Call", cmd, "Create process error");
		return std::nullopt;
	}
	::WaitForSingleObject(process_info.hProcess, 30000);

	std::string pipe_str;
	if (use_pipe && pipe_ret) {
		DWORD read_num = 0;
		DWORD std_num = 0;
		if (::PeekNamedPipe(pipe_read, NULL, 0, NULL, &read_num, NULL) && read_num > 0) {
			char pipe_buffer[PipeBuffSize] = { 0 };
			BOOL read_ret = ::ReadFile(pipe_read, pipe_buffer, read_num, &std_num, NULL);
			if (read_ret) {
				pipe_str = std::string(pipe_buffer, std_num);
			}
		}
	}

	::GetExitCodeProcess(process_info.hProcess, &ret);

	::CloseHandle(process_info.hProcess);
	::CloseHandle(process_info.hThread);
	DebugTrace("Call", cmd, "ret", ret);
	if (use_pipe) {
		DebugTrace("Pipe:", pipe_str);
	}

	if (pipe_read != NULL) {
		::CloseHandle(pipe_read);
	}
	if (pipe_write != NULL) {
		::CloseHandle(pipe_write);
	}

	return pipe_str;
}

Point asst::WinMacro::randPointInRect(const Rect& rect)
{
	int x = 0, y = 0;
	if (rect.width == 0) {
		x = rect.x;
	}
	else {
		int x_rand = std::poisson_distribution<int>(rect.width / 2)(m_rand_engine);

		x = x_rand + rect.x;
	}

	if (rect.height == 0) {
		y = rect.y;
	}
	else {
		int y_rand = std::poisson_distribution<int>(rect.height / 2)(m_rand_engine);
		y = y_rand + rect.y;
	}

	return Point(x, y);
}

bool WinMacro::showWindow()
{
	return ::ShowWindow(m_handle, SW_RESTORE);
}

bool WinMacro::hideWindow()
{
	return ::ShowWindow(m_handle, SW_HIDE);
}

bool WinMacro::click(const Point& p)
{
	int x = p.x * m_control_scale;
	int y = p.y * m_control_scale;
	DebugTrace("Click, raw:", p.x, p.y, "corr:", x, y);

	std::string cur_cmd = StringReplaceAll(m_emulator_info.adb.click, "[x]", std::to_string(x));
	cur_cmd = StringReplaceAll(cur_cmd, "[y]", std::to_string(y));
	return callCmd(cur_cmd, false).has_value();
}

bool WinMacro::click(const Rect& rect)
{
	return click(randPointInRect(rect));
}

bool asst::WinMacro::swipe(const Point& p1, const Point& p2, int duration)
{
	int x1 = p1.x * m_control_scale;
	int y1 = p1.y * m_control_scale;
	int x2 = p2.x * m_control_scale;
	int y2 = p2.y * m_control_scale;
	DebugTrace("Swipe, raw:", p1.x, p1.y, p2.x, p2.y, "corr:", x1, y1, x2, y2);

	std::string cur_cmd = StringReplaceAll(m_emulator_info.adb.swipe, "[x1]", std::to_string(x1));
	cur_cmd = StringReplaceAll(cur_cmd, "[y1]", std::to_string(y1));
	cur_cmd = StringReplaceAll(cur_cmd, "[x2]", std::to_string(x2));
	cur_cmd = StringReplaceAll(cur_cmd, "[y2]", std::to_string(y2));
	if (duration <= 0) {
		cur_cmd = StringReplaceAll(cur_cmd, "[duration]", "");
	}
	else {
		cur_cmd = StringReplaceAll(cur_cmd, "[duration]", std::to_string(duration));
	}

	return callCmd(cur_cmd, false).has_value();
}

bool asst::WinMacro::swipe(const Rect& r1, const Rect& r2, int duration)
{
	return swipe(randPointInRect(r1), randPointInRect(r2), duration);
}

void asst::WinMacro::setControlScale(double scale) noexcept
{
	m_control_scale = scale;
}

std::pair<int, int> asst::WinMacro::getDisplaySize()
{
	static std::pair<int, int> size = std::make_pair(m_emulator_info.adb.display_width, m_emulator_info.adb.display_height);
	return size;

	//return std::make_pair(m_emulator_info.adb.display_width, m_emulator_info.adb.display_height);
}

cv::Mat asst::WinMacro::getImage()
{
	if (callCmd(m_emulator_info.adb.screencap, false).has_value()
		&& callCmd(m_emulator_info.adb.pullscreen, false).has_value()) {
		return cv::imread(m_screen_filename);
	}

	return cv::Mat();
}