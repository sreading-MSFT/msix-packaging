#pragma once
#include <string>

namespace MsixCoreLib
{
    HRESULT CreateKozaniPackage(
        _In_ std::wstring sourceDirectoryPath,
        _In_ std::wstring outputDirectoryPath);
}