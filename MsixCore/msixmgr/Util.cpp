#include "msixmgrLogger.hpp"
#include "..\msixmgrLib\GeneralUtil.hpp"
#include "Util.hpp"
#include <string>
#include <codecvt>
#include <iostream>
#include <filesystem>

std::wstring GetStringResource(UINT resourceId)
{
    HMODULE instance = GetModuleHandle(nullptr);

    WCHAR buffer[2*MAX_PATH] = L"";
    int loadStringRet = LoadStringW(instance, resourceId, buffer, ARRAYSIZE(buffer));
    if (loadStringRet <= 0)
    {
        return std::wstring(L"Failed to load string resource");
    }

    std::wstring stringResource(buffer);

    return stringResource;
}

void WriteTempDirectoryFailureError(_In_ std::wstring temporaryPath, bool createTempDirResult, std::error_code createDirectoryErrorCode)
{
    // Since we're using a GUID, this should almost never happen
    if (!createTempDirResult)
    {
        std::wcout << std::endl;
        std::wcout << "Failed to create temp directory " << temporaryPath << std::endl;
        std::wcout << "This may occur when the directory path already exists. Please try again." << std::endl;
        std::wcout << std::endl;
    }
    if (createDirectoryErrorCode.value() != 0)
    {
        // Again, we expect that the creation of the temp directory will fail very rarely. Output the exception
        // and have the user try again.
        std::wcout << std::endl;
        std::wcout << "Creation of temp directory " << temporaryPath << " failed with error: " << createDirectoryErrorCode.value() << std::endl;
        std::cout << "Error message: " << createDirectoryErrorCode.message() << std::endl;
        std::wcout << "Please try again." << std::endl;
        std::wcout << std::endl;
    }
}

bool CreateTempDirectoryPath(std::wstring tempRootDir, _Out_ std::wstring& newTempPath, _Out_ std::error_code& createDirectoryErrorCode)
{
    // Create a temporary directory to unpack package(s) since we cannot unpack to the CIM directly.
    // Append long path prefix to temporary directory path to handle paths that exceed the maximum path length limit
    std::wstring uniqueIdString;
    RETURN_IF_FAILED(MsixCoreLib::CreateGUIDString(&uniqueIdString));
    std::filesystem::path tempDirPath{ tempRootDir };
    tempDirPath /= uniqueIdString;

    bool createTempDirResult = std::filesystem::create_directory(tempDirPath, createDirectoryErrorCode);

    newTempPath.append(tempDirPath);
    return createTempDirResult;

}

// Create temp directory and log failures to command line.
HRESULT CreateTempDirectory(_Out_ std::wstring& tempDirPath)
{
    std::wstring tempDirectory = std::filesystem::temp_directory_path();
    wchar_t longNameTempDirectoryArray[MAX_PATH];
    RETURN_IF_FAILED(GetLongPathName(tempDirectory.c_str(), longNameTempDirectoryArray, MAX_PATH));
    std::wstring longNameTempDirectory = std::wstring(longNameTempDirectoryArray);

    bool createTempDirResult;
    std::error_code createDirectoryError;
    createTempDirResult = CreateTempDirectoryPath(longNameTempDirectory, tempDirPath, createDirectoryError);
    if (!createTempDirResult || createDirectoryError.value() != 0)
    {
        WriteTempDirectoryFailureError(tempDirPath, createTempDirResult, createDirectoryError);
        return E_UNEXPECTED;
    }
    return S_OK;
}
