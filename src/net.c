/*
 * Rufus: The Reliable USB Formatting Utility
 * Networking functionality (web file download, check for update, etc.)
 * Copyright © 2012-2026 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <wininet.h>
#include <netlistmgr.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <time.h>
#include <virtdisk.h>

#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "bled/bled.h"
#include "dbx/dbx_info.h"

#include "settings.h"

/* Maximum download chunk size, in bytes */
#define DOWNLOAD_BUFFER_SIZE    (10*KB)
/* Default delay between update checks (1 day) */
#define DEFAULT_UPDATE_INTERVAL (24*3600)

DWORD DownloadStatus;
BYTE* fido_script = NULL;
HANDLE update_check_thread = NULL;

extern loc_cmd* selected_locale;
extern HANDLE dialog_handle;
extern BOOL is_x86_64;
extern USHORT NativeMachine;
static DWORD error_code, fido_len = 0;
static BOOL force_update_check = FALSE;
extern const char* efi_archname[ARCH_MAX];

#if defined(__MINGW32__)
#define INetworkListManager_get_IsConnectedToInternet INetworkListManager_IsConnectedToInternet
#endif

static char* GetShortName(const char* url)
{
	static char short_name[128];
	char *p;
	size_t i, len = safe_strlen(url);
	if (len < 5)
		return NULL;

	for (i = len - 2; i > 0; i--) {
		if (url[i] == '/') {
			i++;
			break;
		}
	}
	memset(short_name, 0, sizeof(short_name));
	static_strcpy(short_name, &url[i]);
	// If the URL is followed by a query, remove that part
	// Make sure we detect escaped queries too
	p = strstr(short_name, "%3F");
	if (p != NULL)
		*p = 0;
	p = strstr(short_name, "%3f");
	if (p != NULL)
		*p = 0;
	for (i = 0; i < strlen(short_name); i++) {
		if ((short_name[i] == '?') || (short_name[i] == '#')) {
			short_name[i] = 0;
			break;
		}
	}
	return short_name;
}

static __inline BOOL is_WOW64(void)
{
	BOOL ret = FALSE;
	IsWow64Process(GetCurrentProcess(), &ret);
	return ret;
}

// Open an Internet session
static HINTERNET GetInternetSession(const char* user_agent, BOOL bRetry)
{
	int i;
	char default_agent[64];
	BOOL decodingSupport = TRUE;
	VARIANT_BOOL InternetConnection = VARIANT_FALSE;
	DWORD dwFlags, dwTimeout = NET_SESSION_TIMEOUT, dwProtocolSupport = HTTP_PROTOCOL_FLAG_HTTP2;
	HINTERNET hSession = NULL;
	HRESULT hr = S_FALSE;
	INetworkListManager* pNetworkListManager;
	// Create a NetworkListManager Instance to check the network connection
	IGNORE_RETVAL(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
	hr = CoCreateInstance(&CLSID_NetworkListManager, NULL, CLSCTX_ALL,
		&IID_INetworkListManager, (LPVOID*)&pNetworkListManager);
	if (hr == S_OK) {
		for (i = 0; i <= WRITE_RETRIES; i++) {
			hr = INetworkListManager_get_IsConnectedToInternet(pNetworkListManager, &InternetConnection);
			// INetworkListManager may fail with ERROR_SERVICE_DEPENDENCY_FAIL if the DHCP service
			// is not running, in which case we must fall back to using InternetGetConnectedState().
			// See https://github.com/pbatard/rufus/issues/1801.
			if (hr == HRESULT_FROM_WIN32(ERROR_SERVICE_DEPENDENCY_FAIL)) {
				InternetConnection = InternetGetConnectedState(&dwFlags, 0) ? VARIANT_TRUE : VARIANT_FALSE;
				break;
			}
			if (hr == S_OK || !bRetry)
				break;
			Sleep(1000);
		}
	}
	if (InternetConnection == VARIANT_FALSE) {
		SetLastError(ERROR_INTERNET_DISCONNECTED);
		goto out;
	}
	static_sprintf(default_agent, APPLICATION_NAME "/%d.%d.%d (Windows NT %lu.%lu%s)",
		rufus_version[0], rufus_version[1], rufus_version[2],
		WindowsVersion.Major, WindowsVersion.Minor, is_WOW64() ? "; WOW64" : "");
	hSession = InternetOpenA((user_agent == NULL) ? default_agent : user_agent,
		INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	// Set the timeouts
	InternetSetOptionA(hSession, INTERNET_OPTION_CONNECT_TIMEOUT, (LPVOID)&dwTimeout, sizeof(dwTimeout));
	InternetSetOptionA(hSession, INTERNET_OPTION_SEND_TIMEOUT, (LPVOID)&dwTimeout, sizeof(dwTimeout));
	InternetSetOptionA(hSession, INTERNET_OPTION_RECEIVE_TIMEOUT, (LPVOID)&dwTimeout, sizeof(dwTimeout));
	// Enable gzip and deflate decoding schemes
	InternetSetOptionA(hSession, INTERNET_OPTION_HTTP_DECODING, (LPVOID)&decodingSupport, sizeof(decodingSupport));
	// Enable HTTP/2 protocol support
	InternetSetOptionA(hSession, INTERNET_OPTION_ENABLE_HTTP_PROTOCOL, (LPVOID)&dwProtocolSupport, sizeof(dwProtocolSupport));

out:
	return hSession;
}

/*
 * Download a file or fill a buffer from an URL
 * Mostly taken from http://support.microsoft.com/kb/234913
 * If file is NULL, a buffer is allocated for the download (that needs to be freed by the caller)
 * If hProgressDialog is not NULL, this function will send INIT and EXIT messages
 * to the dialog in question, with WPARAM being set to nonzero for EXIT on success
 * and also attempt to indicate progress using an IDC_PROGRESS control
 * Note that when a buffer is used, the actual size of the buffer is two more than its reported
 * size (with the extra bytes set to 0) to accommodate for calls that need NUL-terminated data.
 */
uint64_t DownloadToFileOrBufferEx(const char* url, const char* file, const char* user_agent,
	BYTE** buffer, HWND hProgressDialog, BOOL bTaskBarProgress)
{
	const char* accept_types[] = {"*/*\0", NULL};
	const char* short_name;
	unsigned char buf[DOWNLOAD_BUFFER_SIZE];
	char hostname[64], urlpath[128], strsize[32];
	BOOL r = FALSE;
	DWORD dwSize, dwWritten, dwDownloaded;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	HINTERNET hSession = NULL, hConnection = NULL, hRequest = NULL;
	URL_COMPONENTSA UrlParts = { sizeof(URL_COMPONENTSA), NULL, 1, (INTERNET_SCHEME)0,
		hostname, sizeof(hostname), 0, NULL, 1, urlpath, sizeof(urlpath), NULL, 1 };
	uint64_t size = 0, total_size = 0;

	ErrorStatus = 0;
	DownloadStatus = 404;
	if (hProgressDialog != NULL)
		UpdateProgressWithInfoInit(hProgressDialog, FALSE);

	assert(url != NULL);
	if (buffer != NULL)
		*buffer = NULL;

	short_name = (file != NULL) ? PathFindFileNameU(file) : PathFindFileNameU(url);

	if (hProgressDialog != NULL) {
		PrintInfo(5000, MSG_085, short_name);
		uprintf("Downloading %s", url);
	}

	if ( (!InternetCrackUrlA(url, (DWORD)safe_strlen(url), 0, &UrlParts))
	  || (UrlParts.lpszHostName == NULL) || (UrlParts.lpszUrlPath == NULL)) {
		uprintf("Unable to decode URL: %s", WindowsErrorString());
		goto out;
	}
	hostname[sizeof(hostname)-1] = 0;

	hSession = GetInternetSession(user_agent, TRUE);
	if (hSession == NULL) {
		uprintf("Could not open Internet session: %s", WindowsErrorString());
		goto out;
	}

	hConnection = InternetConnectA(hSession, UrlParts.lpszHostName, UrlParts.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, (DWORD_PTR)NULL);
	if (hConnection == NULL) {
		uprintf("Could not connect to server %s:%d: %s", UrlParts.lpszHostName, UrlParts.nPort, WindowsErrorString());
		goto out;
	}

	hRequest = HttpOpenRequestA(hConnection, "GET", UrlParts.lpszUrlPath, NULL, NULL, accept_types,
		INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS |
		INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_HYPERLINK |
		((UrlParts.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_FLAG_SECURE : 0), (DWORD_PTR)NULL);
	if (hRequest == NULL) {
		uprintf("Could not open URL %s: %s", url, WindowsErrorString());
		goto out;
	}

	// If we are querying the GitHub API, we need to enable raw content
	if (strstr(url, "api.github.com") != NULL && !HttpAddRequestHeadersA(hRequest,
		"Accept: application/vnd.github.v3.raw", (DWORD)-1, HTTP_ADDREQ_FLAG_ADD)) {
		uprintf("Unable to enable raw content from GitHub API: %s", WindowsErrorString());
		goto out;
	}
	// Must use "Accept-Encoding: identity" to get the file size
	// This is needed for GitHub as the Microsoft HTTP APIs can't seem to read content-length for
	// compressed content from GitHub, and using "identity" disables compression.
	HttpSendRequestA(hRequest, "Accept-Encoding: identity", -1L, NULL, 0);

	// Get the file size
	dwSize = sizeof(DownloadStatus);
	HttpQueryInfoA(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, (LPVOID)&DownloadStatus, &dwSize, NULL);
	if (DownloadStatus != 200) {
		error_code = ERROR_INTERNET_ITEM_NOT_FOUND;
		SetLastError(RUFUS_ERROR(error_code));
		uprintf("%s '%s': %d", (DownloadStatus == 404) ? "File not found" : "Unable to access file", url, DownloadStatus);
		goto out;
	}
	dwSize = sizeof(strsize);
	if (!HttpQueryInfoA(hRequest, HTTP_QUERY_CONTENT_LENGTH, (LPVOID)strsize, &dwSize, NULL)) {
		uprintf("Unable to retrieve file length: %s", WindowsErrorString());
		goto out;
	}
	total_size = strtoull(strsize, NULL, 10);
	if (hProgressDialog != NULL) {
		char msg[128];
		uprintf("File length: %s", SizeToHumanReadable(total_size, FALSE, FALSE));
		if (right_to_left_mode)
			static_sprintf(msg, "(%s) %s", SizeToHumanReadable(total_size, FALSE, FALSE), GetShortName(url));
		else
			static_sprintf(msg, "%s (%s)", GetShortName(url), SizeToHumanReadable(total_size, FALSE, FALSE));
		PrintStatus(5000, MSG_085, msg);
	}

	if (file != NULL) {
		hFile = CreateFileU(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			uprintf("Unable to create file '%s': %s", short_name, WindowsErrorString());
			goto out;
		}
	} else {
		if (buffer == NULL) {
			uprintf("No buffer pointer provided for download");
			goto out;
		}
		// Allocate one extra byte, so that caller can rely on NUL-terminated text if needed
		*buffer = calloc((size_t)total_size + 2, 1);
		if (*buffer == NULL) {
			uprintf("Could not allocate buffer for download");
			goto out;
		}
	}

	// Keep checking for data until there is nothing left.
	while (1) {
		// User may have cancelled the download
		if (IS_ERROR(ErrorStatus))
			goto out;
		if (!InternetReadFile(hRequest, buf, sizeof(buf), &dwDownloaded) || (dwDownloaded == 0))
			break;
		if (hProgressDialog != NULL)
			UpdateProgressWithInfo(OP_NOOP, MSG_241, size, total_size);
		if (file != NULL) {
			if (!WriteFile(hFile, buf, dwDownloaded, &dwWritten, NULL)) {
				uprintf("Error writing file '%s': %s", short_name, WindowsErrorString());
				goto out;
			} else if (dwDownloaded != dwWritten) {
				uprintf("Error writing file '%s': Only %d/%d bytes written", short_name, dwWritten, dwDownloaded);
				goto out;
			}
		} else {
			memcpy(&(*buffer)[size], buf, dwDownloaded);
		}
		size += dwDownloaded;
	}

	if (size != total_size) {
		uprintf("Could not download complete file - read: %lld bytes, expected: %lld bytes", size, total_size);
		ErrorStatus = RUFUS_ERROR(ERROR_WRITE_FAULT);
		goto out;
	} else {
		DownloadStatus = 200;
		r = TRUE;
		if (hProgressDialog != NULL) {
			UpdateProgressWithInfo(OP_NOOP, MSG_241, total_size, total_size);
			uprintf("Successfully downloaded '%s'", short_name);
		}
	}

out:
	error_code = GetLastError();
	if (hFile != INVALID_HANDLE_VALUE) {
		// Force a flush - May help with the PKI API trying to process downloaded updates too early...
		FlushFileBuffers(hFile);
		CloseHandle(hFile);
	}
	if (!r) {
		if (file != NULL)
			DeleteFileU(file);
		if (buffer != NULL)
			safe_free(*buffer);
	}
	if (hRequest)
		InternetCloseHandle(hRequest);
	if (hConnection)
		InternetCloseHandle(hConnection);
	if (hSession)
		InternetCloseHandle(hSession);

	SetLastError(error_code);
	return r ? size : 0;
}

// Download and validate a signed file. The file must have a corresponding '.sig' on the server.
DWORD DownloadSignedFile(const char* url, const char* file, HWND hProgressDialog, BOOL bPromptOnError)
{
	char* url_sig = NULL;
	BYTE *buf = NULL, *sig = NULL;
	DWORD buf_len = 0, sig_len = 0;
	DWORD ret = 0;
	HANDLE hFile = INVALID_HANDLE_VALUE;

	assert(url != NULL);

	url_sig = malloc(strlen(url) + 5);
	if (url_sig == NULL) {
		uprintf("Could not allocate signature URL");
		goto out;
	}
	strcpy(url_sig, url);
	strcat(url_sig, ".sig");

	buf_len = (DWORD)DownloadToFileOrBuffer(url, NULL, &buf, hProgressDialog, FALSE);
	if (buf_len == 0)
		goto out;
	sig_len = (DWORD)DownloadToFileOrBuffer(url_sig, NULL, &sig, NULL, FALSE);
	if ((sig_len != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature(buf, buf_len, sig, sig_len))) {
		uprintf("FATAL: Download signature is invalid ✗");
		DownloadStatus = 403;	// Forbidden
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE));
		SendMessage(GetDlgItem(hProgressDialog, IDC_PROGRESS), PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
		SetTaskbarProgressState(TASKBAR_ERROR);
		goto out;
	}

	uprintf("Download signature is valid ✓");
	DownloadStatus = 206;	// Partial content
	hFile = CreateFileU(file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		uprintf("Unable to create file '%s': %s", PathFindFileNameU(file), WindowsErrorString());
		goto out;
	}
	if (!WriteFile(hFile, buf, buf_len, &ret, NULL)) {
		uprintf("Error writing file '%s': %s", PathFindFileNameU(file), WindowsErrorString());
		ret = 0;
		goto out;
	} else if (ret != buf_len) {
		uprintf("Error writing file '%s': Only %d/%d bytes written", PathFindFileNameU(file), ret, buf_len);
		ret = 0;
		goto out;
	}
	DownloadStatus = 200;	// Full content

out:
	if (hProgressDialog != NULL)
		SendMessage(hProgressDialog, UM_PROGRESS_EXIT, (WPARAM)ret, 0);
	if ((bPromptOnError) && (DownloadStatus != 200)) {
		PrintInfo(0, MSG_242);
		SetLastError(error_code);
		Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_044), IS_ERROR(ErrorStatus) ? StrError(ErrorStatus, FALSE) : WindowsErrorString());
	}
	safe_closehandle(hFile);
	free(url_sig);
	free(buf);
	free(sig);
	return ret;
}

/* Threaded download */
typedef struct {
	const char* url;
	const char* file;
	HWND hProgressDialog;
	BOOL bPromptOnError;
} DownloadSignedFileThreadArgs;

static DWORD WINAPI DownloadSignedFileThread(LPVOID param)
{
	DownloadSignedFileThreadArgs* args = (DownloadSignedFileThreadArgs*)param;
	ExitThread(DownloadSignedFile(args->url, args->file, args->hProgressDialog, args->bPromptOnError));
}

HANDLE DownloadSignedFileThreaded(const char* url, const char* file, HWND hProgressDialog, BOOL bPromptOnError)
{
	static DownloadSignedFileThreadArgs args;
	args.url = url;
	args.file = file;
	args.hProgressDialog = hProgressDialog;
	args.bPromptOnError = bPromptOnError;
	return CreateThread(NULL, 0, DownloadSignedFileThread, &args, 0, NULL);
}

static __inline uint64_t to_uint64_t(uint16_t x[3]) {
	int i;
	uint64_t ret = 0;
	for (i = 0; i < 3; i++)
		ret = (ret << 16) + x[i];
	return ret;
}

BOOL UseLocalDbx(int arch)
{
	char reg_name[32];
	static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[arch]);
	return (uint64_t)ReadSetting64(reg_name) > dbx_info[arch - 1].timestamp;
}

static void CheckForDBXUpdates(int verbose)
{
	int i, r;
	char reg_name[32], timestamp_url[256], path[MAX_PATH];
	char *p, *c, *rep, *buf = NULL;
	struct tm t = { 0 };
	uint64_t size, timestamp;
	BOOL already_prompted = FALSE;

	for (i = 0; i < ARRAYSIZE(dbx_info); i++) {
		// Get the epoch of the last commit
		timestamp = 0;
		static_strcpy(timestamp_url, dbx_info[i].url);
		p = strstr(timestamp_url, "contents/");
		if (p == NULL)
			continue;
		*p = 0;
		rep = replace_char(&p[9], '/', "%2F");
		static_strcat(timestamp_url, "commits?path=");
		static_strcat(timestamp_url, rep);
		free(rep);
		static_strcat(timestamp_url, "&page=1&per_page=1");
		vuprintf("Querying %s for DBX update timestamp", timestamp_url);
		size = DownloadToFileOrBuffer(timestamp_url, NULL, (BYTE**)&buf, NULL, FALSE);
		if (size == 0)
			continue;
		// Assumes that the GitHub JSON commit dates are of the form:
		// "date":[ ]*"2025-02-24T20:20:22Z"
		p = strstr(buf, "\"date\":");
		if (p == NULL) {
			safe_free(buf);
			continue;
		}
		c = &p[7];
		while (*c == ' ' || *c == '"')
			c++;
		p = c;
		while (*c != '"' && *c != '\0')
			c++;
		*c = 0;
		// "Thank you, X3J11 ANSI committee, for introducing the well thought through 'struct tm'", said ABSOLUTELY NOONE ever!
		r = sscanf(p, "%d-%d-%dT%d:%d:%dZ", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec);
		safe_free(buf);
		if (r != 6)
			continue;
		t.tm_year -= 1900;
		t.tm_mon -= 1;
		timestamp = _mkgmtime64(&t);
		vuprintf("DBX update timestamp is %" PRId64, timestamp);
		static_sprintf(reg_name, "DBXTimestamp_%s", efi_archname[i + 1]);
		// Check if we have an external DBX that is newer than embedded/last downloaded
		if (timestamp <= MAX(dbx_info[i].timestamp, (uint64_t)ReadSetting64(reg_name)))
			continue;
		if (!already_prompted) {
			r = Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_353), lmprintf(MSG_354));
			already_prompted = TRUE;
			if (r != IDYES)
				break;
			IGNORE_RETVAL(_chdirU(app_data_dir));
			IGNORE_RETVAL(_mkdir(FILES_DIR));
			IGNORE_RETVAL(_chdir(FILES_DIR));
		}
		static_sprintf(path, "%s\\%s\\dbx_%s.bin", app_data_dir, FILES_DIR, efi_archname[i + 1]);
		if (DownloadToFileOrBuffer(dbx_info[i].url, path, NULL, NULL, FALSE) != 0) {
			WriteSetting64(reg_name, timestamp);
			uprintf("Saved %s as 'dbx_%s.bin'", dbx_info[i].url, efi_archname[i + 1]);
		} else
			uprintf("WARNING: Failed to download %s", dbx_info[i].url);
	}
}

/*
 * Extract a JSON string value by key (handles basic escape sequences).
 * Returns a newly allocated string, or NULL on failure. Caller must free.
 */
static char* extract_json_string_value(const char* json, const char* key)
{
	char keybuf[128];
	char *p, *end, *result;
	size_t len;

	static_sprintf(keybuf, "\"%s\": \"", key);
	p = strstr(json, keybuf);
	if (p == NULL)
		return NULL;

	p += safe_strlen(keybuf);

	// Count unescaped length
	len = 0;
	end = p;
	while (*end) {
		if (*end == '\\') {
			end++;
			if (*end) { len++; end++; }
			continue;
		}
		if (*end == '\"')
			break;
		len++;
		end++;
	}
	if (*end != '\"')
		return NULL;

	result = (char*)malloc(len + 1);
	if (result == NULL)
		return NULL;

	// Unescape
	len = 0;
	while (*p && *p != '\"') {
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 'n': result[len++] = '\n'; break;
			case 'r': result[len++] = '\r'; break;
			case 't': result[len++] = '\t'; break;
			case '\"': result[len++] = '\"'; break;
			case '\\': result[len++] = '\\'; break;
			default: result[len++] = *p; break;
			}
			if (*p) p++;
		} else {
			result[len++] = *p++;
		}
	}
	result[len] = 0;

	return result;
}

/*
 * Background thread to check for updates (including UEFI DBX updates)
 */
static DWORD WINAPI CheckForUpdatesThread(LPVOID param)
{
	BOOL releases_only = TRUE, found_new_version = FALSE;
	int status = 0;
	const char* server_url = RUFUS_URL "/";
	int i, j, k, max_channel, verbose = 0, verpos[4];
	static const char* channel[] = { "release", "beta", "test" };		// release channel
	const char* accept_types[] = { "*/*\0", NULL };
	char* buf = NULL;
	char agent[64], hostname[64], urlpath[128], sigpath[256];
	DWORD dwSize, dwDownloaded, dwTotalSize, dwStatus;
	BYTE *sig = NULL;
	HINTERNET hSession = NULL, hConnection = NULL, hRequest = NULL;
	URL_COMPONENTSA UrlParts = { sizeof(URL_COMPONENTSA), NULL, 1, (INTERNET_SCHEME)0,
		hostname, sizeof(hostname), 0, NULL, 1, urlpath, sizeof(urlpath), NULL, 1 };
	SYSTEMTIME ServerTime, LocalTime;
	FILETIME FileTime;
	int64_t local_time = 0, reg_time, server_time, update_interval;
	verbose = ReadSetting32(SETTING_VERBOSE_UPDATES);
	// Without this the FileDialog will produce error 0x8001010E when compiled for Vista or later
	IGNORE_RETVAL(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
	// Unless the update was forced, wait a while before performing the update check
	if (!force_update_check) {
		// It would of course be a lot nicer to use a timer and wake the thread, but my
		// development time is limited and this is FASTER to implement.
		do {
			for (i = 0; ( i < 30) && (!force_update_check); i++)
				Sleep(500);
		} while ((!force_update_check) && ((op_in_progress || (dialog_showing > 0))));
		if (!force_update_check) {
			if ((ReadSetting32(SETTING_UPDATE_INTERVAL) == -1)) {
				vuprintf("Check for updates disabled, as per settings.");
				goto out;
			}
			reg_time = ReadSetting64(SETTING_LAST_UPDATE);
			update_interval = (int64_t)ReadSetting32(SETTING_UPDATE_INTERVAL);
			if (update_interval == 0) {
				WriteSetting32(SETTING_UPDATE_INTERVAL, DEFAULT_UPDATE_INTERVAL);
				update_interval = DEFAULT_UPDATE_INTERVAL;
			}
			GetSystemTime(&LocalTime);
			if (!SystemTimeToFileTime(&LocalTime, &FileTime))
				goto out;
			local_time = ((((int64_t)FileTime.dwHighDateTime) << 32) + FileTime.dwLowDateTime) / 10000000;
			vvuprintf("Local time: %" PRId64, local_time);
			if (local_time < reg_time + update_interval) {
				vuprintf("Next update check in %" PRId64 " seconds.", reg_time + update_interval - local_time);
				goto out;
			}
		}
	}

	// Perform the DBX Update check
	PrintInfoDebug(3000, MSG_352);
	CheckForDBXUpdates(verbose);

	// RuBeRoID: Check for updates via GitHub API
	PrintInfoDebug(3000, MSG_243);
	status++;	// 1

	hSession = GetInternetSession(NULL, FALSE);
	if (hSession == NULL)
		goto out;

	hConnection = InternetConnectA(hSession, "api.github.com", INTERNET_DEFAULT_HTTPS_PORT,
		NULL, NULL, INTERNET_SERVICE_HTTP, 0, (DWORD_PTR)NULL);
	if (hConnection == NULL)
		goto out;

	vuprintf("Using GitHub API for the update check");

	status++;	// 2

	static_sprintf(agent, APPLICATION_NAME "/%d.%d.%d",
		rufus_version[0], rufus_version[1], rufus_version[2]);

	hRequest = HttpOpenRequestA(hConnection, "GET", "/repos/" RUFUS_REPO "/releases/latest",
		NULL, NULL, accept_types,
		INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_HYPERLINK | INTERNET_FLAG_SECURE,
		(DWORD_PTR)NULL);
	if (hRequest == NULL) {
		uprintf("Unable to send request: %s", WindowsErrorString());
		goto out;
	}
	{
		char headers[256];
		static_sprintf(headers, "Accept: application/vnd.github.v3+json\r\nUser-Agent: %s", agent);
		if (!HttpSendRequestA(hRequest, headers, -1L, NULL, 0)) {
			uprintf("Failed to send request: %s", WindowsErrorString());
			goto out;
		}
	}

	dwSize = sizeof(dwStatus);
	dwStatus = 404;
	HttpQueryInfoA(hRequest, HTTP_QUERY_STATUS_CODE|HTTP_QUERY_FLAG_NUMBER, (LPVOID)&dwStatus, &dwSize, NULL);
	if (dwStatus != 200) {
		vuprintf("GitHub API returned status %lu", dwStatus);
		goto out;
	}

	dwSize = sizeof(dwTotalSize);
	if (!HttpQueryInfoA(hRequest, HTTP_QUERY_CONTENT_LENGTH|HTTP_QUERY_FLAG_NUMBER, (LPVOID)&dwTotalSize, &dwSize, NULL))
		goto out;

	buf = (char*)calloc(dwTotalSize + 1, 1);
	if (buf == NULL)
		goto out;
	if (!InternetReadFile(hRequest, buf, dwTotalSize, &dwDownloaded) || (dwDownloaded != dwTotalSize))
		goto out;

	vuprintf("Successfully downloaded GitHub release info (%d bytes)", dwTotalSize);

	// Parse tag_name from JSON
	{
		char tag[64] = "";
		char *p, *ver_str;
		int n;

		p = strstr(buf, "\"tag_name\": \"");
		if (p != NULL) {
			p += 13;
			for (n = 0; n < (int)sizeof(tag) - 1 && *p && *p != '\"'; n++)
				tag[n] = *p++;
			tag[n] = 0;
		}
		if (tag[0] == 0) {
			vuprintf("Could not find tag_name in GitHub response");
			goto out;
		}
		vuprintf("Latest release tag: %s", tag);

		// Parse version from tag (e.g., "v4.15-ruberoid" -> "4.15" -> [4,15,0])
		ver_str = tag;
		if (ver_str[0] == 'v' || ver_str[0] == 'V')
			ver_str++;
		p = strchr(ver_str, '-');
		if (p != NULL)
			*p = 0;
		for (n = 0; n < 3; n++)
			update.version[n] = 0;
		n = 0;
		p = strtok(ver_str, ".");
		while (p != NULL && n < 3) {
			update.version[n++] = (uint16_t)atoi(p);
			p = strtok(NULL, ".");
		}
	}

	// Parse browser_download_url from JSON
	{
		char url[1024] = "";
		char *p;
		int n;

		p = strstr(buf, "\"browser_download_url\": \"");
		if (p != NULL) {
			p += 24;
			for (n = 0; n < (int)sizeof(url) - 1 && *p && *p != '\"'; n++)
				url[n] = *p++;
			url[n] = 0;
		}
		if (url[0] == 0) {
			vuprintf("Could not find download URL in GitHub response");
			goto out;
		}
		safe_free(update.download_url);
		update.download_url = strdup(url);
	}

	// Parse body (release notes)
	safe_free(update.release_notes);
	update.release_notes = extract_json_string_value(buf, "body");
	if (update.release_notes == NULL) {
		update.release_notes = (char*)malloc(128);
		if (update.release_notes != NULL)
			safe_sprintf(update.release_notes, 128, "See " RUFUS_URL_RELEASES " for details.");
	}

	// We support all Windows versions that Rufus supports
	update.platform_min[0] = 5;
	update.platform_min[1] = 2;

	status++;	// 3

	vuprintf("UPDATE DATA:");
	vuprintf("  version: %d.%d.%d", update.version[0], update.version[1], update.version[2]);
	vuprintf("  url: %s", update.download_url);

	found_new_version = (to_uint64_t(update.version) > to_uint64_t(rufus_version)) || (force_update);
	uprintf("N%sew version found%c", found_new_version ? "" : "o n", found_new_version ? '!' : '.');

out:
	safe_free(buf);
	safe_free(sig);
	if (hRequest)
		InternetCloseHandle(hRequest);
	if (hConnection)
		InternetCloseHandle(hConnection);
	if (hSession)
		InternetCloseHandle(hSession);
	switch (status) {
	case 1:
		PrintInfoDebug(3000, MSG_244);
		break;
	case 2:
		PrintInfoDebug(3000, MSG_245);
		break;
	case 3:
	case 4:
		PrintInfo(3000, found_new_version ? MSG_246 : MSG_247);
	default:
		break;
	}
	// Start the new download after cleanup
	if (found_new_version) {
		// User may have started an operation while we were checking
		while ((!force_update_check) && (op_in_progress || (dialog_showing > 0))) {
			Sleep(15000);
		}
		DownloadNewVersion();
	} else if (force_update_check) {
		PostMessage(hMainDialog, UM_NO_UPDATE, 0, 0);
	}
	force_update_check = FALSE;
	update_check_thread = NULL;
	CoUninitialize();
	ExitThread(0);
}

/*
 * Initiate a check for updates. If force is true, ignore the wait period
 */
BOOL CheckForUpdates(BOOL force)
{
	force_update_check = force;
	if (update_check_thread != NULL)
		return FALSE;

	update_check_thread = CreateThread(NULL, 0, CheckForUpdatesThread, NULL, 0, NULL);
	if (update_check_thread == NULL) {
		uprintf("Unable to start update check thread");
		return FALSE;
	}
	return TRUE;
}

/*
 * Download an ISO — RuBeRoID replacement for Fido
 * Fetches iso_links.json from GitHub repo, parses it, downloads ISO.
 */

typedef struct {
	int os_id;
	int arch;
} download_params;

static DWORD WINAPI DownloadISOThread(LPVOID param)
{
	download_params* dp = (download_params*)param;
	char *json_url, *iso_url, *end, *p;
	const char* arch_str;
	BYTE *json_buffer;
	DWORD json_len;
	char search_key[24];
	IMG_SAVE img_save;

	json_url = NULL;
	iso_url = NULL;
	json_buffer = NULL;
	memset(&img_save, 0, sizeof(img_save));

	dialog_showing++;
	IGNORE_RETVAL(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

	json_url = malloc(MAX_PATH);
	if (json_url == NULL) goto out;
	safe_sprintf(json_url, MAX_PATH, "%s/%s", RUFUS_REPO_RAW, "iso_links.json");

	PrintInfo(0, MSG_148);
	SendMessage(hProgress, PBM_SETSTATE, (WPARAM)PBST_NORMAL, 0);

	json_len = (DWORD)DownloadToFileOrBuffer(json_url, NULL, &json_buffer, hMainDialog, FALSE);
	if (json_len == 0) {
		uprintf("Failed to download link data from GitHub");
		Notification(MB_ICONERROR | MB_CLOSE, lmprintf(MSG_194, "iso_links.json"),
			lmprintf(MSG_043, WindowsErrorString()));
		goto out;
	}
	json_buffer[json_len] = 0;

	arch_str = (dp->arch == 1) ? "x86" : "x64";
	static_sprintf(search_key, "\"%d_%s\": \"", dp->os_id, arch_str);

	p = strstr((char*)json_buffer, search_key);
	if (p == NULL) {
		uprintf("Download link for %s not found in GitHub data", search_key);
		Notification(MB_ICONERROR | MB_CLOSE, lmprintf(MSG_212),
			"Download link not available.\nRequest via Telegram bot.");
		goto out;
	}
	p += strlen(search_key);
	end = strchr(p, '"');
	if (end == NULL) { uprintf("JSON parse error: unterminated URL"); goto out; }
	*end = 0;
	iso_url = strdup(p);
	*end = '"';

	if (iso_url == NULL || strlen(iso_url) < 10) {
		uprintf("Invalid ISO URL"); goto out;
	}
	uprintf("Got ISO URL (%.80s...)", iso_url);

	EXT_DECL(img_ext, GetShortName(iso_url), __VA_GROUP__("*.iso"), __VA_GROUP__(lmprintf(MSG_036)));
	img_save.Type = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
	img_save.ImagePath = FileDialog(TRUE, NULL, &img_ext, NULL);
	if (img_save.ImagePath == NULL)
		goto out;

	SendMessage(hMainDialog, UM_PROGRESS_INIT, 0, 0);
	ErrorStatus = 0;
	SendMessage(hMainDialog, UM_TIMER_START, 0, 0);

	if (DownloadToFileOrBuffer(iso_url, img_save.ImagePath, NULL, hMainDialog, TRUE) == 0) {
		SendMessage(hMainDialog, UM_PROGRESS_EXIT, 0, 0);
		if (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED) {
			uprintf("Download cancelled by user");
			Notification(MB_ICONINFORMATION | MB_CLOSE, lmprintf(MSG_211), lmprintf(MSG_041));
			PrintInfo(0, MSG_211);
		} else {
			Notification(MB_ICONERROR | MB_CLOSE, lmprintf(MSG_194, GetShortName(iso_url)),
				lmprintf(MSG_043, WindowsErrorString()));
			PrintInfo(0, MSG_212);
		}
	} else {
		image_path = safe_strdup(img_save.ImagePath);
		PostMessage(hMainDialog, UM_SELECT_ISO, 0, 0);
	}

out:
	safe_free(img_save.ImagePath);
	free(iso_url);
	safe_free(json_buffer);
	free(json_url);
	free(dp);
	SendMessage(hMainDialog, UM_ENABLE_CONTROLS, 0, 0);
	dialog_showing--;
	CoUninitialize();
	ExitThread(0);
}

BOOL DownloadISO()
{
	int os_id, arch, ver_result, arch_result;
	download_params* dp;
	TASKDIALOGCONFIG tdc;
	TASKDIALOG_BUTTON ver_buttons[2];
	TASKDIALOG_BUTTON arch_buttons[2];

	ver_result = 0;
	arch_result = 0;

	// Version selection dialog
	memset(&tdc, 0, sizeof(tdc));
	tdc.cbSize = sizeof(tdc);
	tdc.hwndParent = hMainDialog;
	tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
	tdc.pszWindowTitle = L"RuBeRoID";
	tdc.pszMainInstruction = L"Select Windows Version";
	tdc.pszContent = L"Choose the version to download.\nLanguage: Russian only.";
	tdc.nDefaultRadioButton = 0;
	tdc.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
	ver_buttons[0].nButtonID = 0;
	ver_buttons[0].pszButtonText = L"Windows 11";
	ver_buttons[1].nButtonID = 1;
	ver_buttons[1].pszButtonText = L"Windows 10";
	tdc.pRadioButtons = ver_buttons;
	tdc.cRadioButtons = 2;

	if (TaskDialogIndirect(&tdc, NULL, &ver_result, NULL) != S_OK)
		return FALSE;
	os_id = (ver_result == 0) ? 11 : 10;

	if (os_id == 10) {
		// Architecture selection for Win10
		memset(&tdc, 0, sizeof(tdc));
		tdc.cbSize = sizeof(tdc);
		tdc.hwndParent = hMainDialog;
		tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
		tdc.pszWindowTitle = L"RuBeRoID";
		tdc.pszMainInstruction = L"Select Architecture";
		tdc.pszContent = L"Choose architecture for Windows 10.";
		tdc.nDefaultRadioButton = 0;
		tdc.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
		arch_buttons[0].nButtonID = 0;
		arch_buttons[0].pszButtonText = L"x64 (64-bit)";
		arch_buttons[1].nButtonID = 1;
		arch_buttons[1].pszButtonText = L"x86 (32-bit)";
		tdc.pRadioButtons = arch_buttons;
		tdc.cRadioButtons = 2;

		if (TaskDialogIndirect(&tdc, NULL, &arch_result, NULL) != S_OK)
			return FALSE;
		arch = arch_result;
	} else {
		arch = 0;	// Win11 is x64 only
	}

	dp = (download_params*)malloc(sizeof(download_params));
	if (dp == NULL) return FALSE;
	dp->os_id = os_id;
	dp->arch = arch;

	if (CreateThread(NULL, 0, DownloadISOThread, dp, 0, NULL) == NULL) {
		uprintf("Unable to start ISO download thread");
		ErrorStatus = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		SendMessage(hMainDialog, UM_ENABLE_CONTROLS, 0, 0);
		free(dp);
		return FALSE;
	}
	return TRUE;
}

BOOL IsDownloadable(const char* url)
{
	DWORD dwSize, dwTotalSize = 0;
	const char* accept_types[] = { "*/*\0", NULL };
	char hostname[64], urlpath[128];
	HINTERNET hSession = NULL, hConnection = NULL, hRequest = NULL;
	URL_COMPONENTSA UrlParts = { sizeof(URL_COMPONENTSA), NULL, 1, (INTERNET_SCHEME)0,
		hostname, sizeof(hostname), 0, NULL, 1, urlpath, sizeof(urlpath), NULL, 1 };

	if (url == NULL)
		return FALSE;

	ErrorStatus = 0;
	DownloadStatus = 404;

	if ((!InternetCrackUrlA(url, (DWORD)safe_strlen(url), 0, &UrlParts))
		|| (UrlParts.lpszHostName == NULL) || (UrlParts.lpszUrlPath == NULL))
		goto out;
	hostname[sizeof(hostname) - 1] = 0;

	// Open an Internet session
	hSession = GetInternetSession(NULL, FALSE);
	if (hSession == NULL)
		goto out;

	hConnection = InternetConnectA(hSession, UrlParts.lpszHostName, UrlParts.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, (DWORD_PTR)NULL);
	if (hConnection == NULL)
		goto out;

	hRequest = HttpOpenRequestA(hConnection, "GET", UrlParts.lpszUrlPath, NULL, NULL, accept_types,
		INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS |
		INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_HYPERLINK |
		((UrlParts.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_FLAG_SECURE : 0), (DWORD_PTR)NULL);
	if (hRequest == NULL)
		goto out;

	// Must use "Accept-Encoding: identity" to get the file size
	HttpSendRequestA(hRequest, "Accept-Encoding: identity", -1L, NULL, 0);

	// Get the file size
	dwSize = sizeof(DownloadStatus);
	HttpQueryInfoA(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, (LPVOID)&DownloadStatus, &dwSize, NULL);
	if (DownloadStatus != 200)
		goto out;
	dwSize = sizeof(dwTotalSize);
	HttpQueryInfoA(hRequest, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, (LPVOID)&dwTotalSize, &dwSize, NULL);

out:
	if (hRequest)
		InternetCloseHandle(hRequest);
	if (hConnection)
		InternetCloseHandle(hConnection);
	if (hSession)
		InternetCloseHandle(hSession);

	return (dwTotalSize > 0);
}
