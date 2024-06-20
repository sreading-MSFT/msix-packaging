#pragma once
#include <windows.h>
#include "AppxPackaging.hpp"
#include <winmeta.h>
#include <string.h>

/// Helper to get string resource
///
/// @param resourceId - resource ID, these should be listed in resource.h
/// @return string for the resource, resolved from the stringtable defined in msixmgr.rc
std::wstring GetStringResource(UINT resourceId);

void WriteTempDirectoryFailureError(_In_ std::wstring temporaryPath, bool createTempDirResult, std::error_code createDirectoryErrorCode);

bool CreateTempDirectoryPath(std::wstring tempRootDir, _Out_ std::wstring& newTempPath, _Out_ std::error_code& createDirectoryErrorCode);

// Create temp directory and log failures to command line.
HRESULT CreateTempDirectory(_Out_ std::wstring& tempDirPath);
