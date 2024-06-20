#include "MSIXWindows.hpp"
#include "Constants.hpp"
#include "CreateKozaniPackageProvider.hpp"
#include "ApplyACLsProvider.hpp"

#include <TraceLoggingProvider.h>
#include "msixmgrLogger.hpp"
#include "..\msixmgrLib\GeneralUtil.hpp"
#include <shlobj_core.h>
#include <CommCtrl.h>
#include <set>
#include <iostream>
#include <filesystem>
#include "MsixErrors.hpp"
#include <MrmResourceIndexer.h>
#include <fstream>
#include "CommandLineInterface.hpp"
#include "Util.hpp"

using namespace MsixCoreLib;
using namespace std;

namespace MsixCoreLib
{
    HRESULT GetResourceValueStringFromCandidate(IMsixElement* candidateElement, std::wstring& resourceValue)
    {
        ComPtr<IMsixElementEnumerator> valueEnum;
        RETURN_IF_FAILED(candidateElement->GetElements(priLocalizedValueQuery.c_str(), &valueEnum));
        BOOL hasCurrentValueElement = FALSE;
        RETURN_IF_FAILED(valueEnum->GetHasCurrent(&hasCurrentValueElement));
        if (!hasCurrentValueElement)
        {
            return HRESULT_FROM_WIN32(ERROR_XML_PARSE_ERROR);
        }
        ComPtr<IMsixElement> valueElement;
        RETURN_IF_FAILED(valueEnum->GetCurrent(&valueElement));

        RETURN_IF_FAILED(GetTextValueFromElement(valueElement.Get(), resourceValue));
        if (resourceValue.empty())
        {
            return HRESULT_FROM_WIN32(ERROR_XML_PARSE_ERROR);
        }

        return S_OK;
    }

    HRESULT AppendToQualifierString(IMsixElement* qualifierElement, std::wstring& qualifierString)
    {
        std::wstring valueAttribute{};
        RETURN_IF_FAILED(GetAttributeValueFromElement(qualifierElement, priValueAttribute, valueAttribute));

        std::wstring nameAttribute{};
        RETURN_IF_FAILED(GetAttributeValueFromElement(qualifierElement, priNameAttribute, nameAttribute));

        // Build full resource qualifier value from attributes. Examples of final strings: L"language-en-US_scale-100_contrast-standard", L"language-fr-CA"
        std::wstring currentResourceQualifier{};
        if ((nameAttribute.compare(priLanguageQualifierName) != 0) &&
            (nameAttribute.compare(priScaleQualifierName) != 0) &&
            (nameAttribute.compare(priContrastQualifierName) != 0) &&
            (nameAttribute.compare(priStringTargetSizeTypeName) != 0) &&
            (nameAttribute.compare(priStringAlternateFormTypeName) != 0))
        {
            return HRESULT_FROM_WIN32(ERROR_XML_PARSE_ERROR);
        }

        std::transform(nameAttribute.begin(), nameAttribute.end(), nameAttribute.begin(), ::towlower);
        if (!qualifierString.empty())
        {
            qualifierString += L"_";
        }
        qualifierString += nameAttribute + L"-" + valueAttribute;
        return S_OK;
    }

    HRESULT GetResourceQualifierStringFromCandidate(IMsixElement* candidateElement, std::wstring& resourceQualifier)
    {
        ComPtr<IMsixElementEnumerator> qualifiersEnum;
        RETURN_IF_FAILED(candidateElement->GetElements(priQualifiersQuery.c_str(), &qualifiersEnum));
        BOOL hasCurrentQualifierElement = FALSE;
        RETURN_IF_FAILED(qualifiersEnum->GetHasCurrent(&hasCurrentQualifierElement));
        while (hasCurrentQualifierElement)
        {
            ComPtr<IMsixElement> qualifierElement;
            RETURN_IF_FAILED(qualifiersEnum->GetCurrent(&qualifierElement));

            RETURN_IF_FAILED(AppendToQualifierString(qualifierElement.Get(), resourceQualifier));

            RETURN_IF_FAILED(qualifiersEnum->MoveNext(&hasCurrentQualifierElement));
        }
        if (resourceQualifier.empty())
        {
            return HRESULT_FROM_WIN32(ERROR_XML_PARSE_ERROR);
        }

        return S_OK;
    }

    // Create a version of the resource pri file that only contains resources in the namedResources set, which is the set of resources required by the manifest.
    HRESULT CreateKozaniResourcePriWithNamedResources(
        std::wstring resourcesPriFilePath,
        std::wstring outputDirectoryPath,
        std::wstring packageName,
        std::set<std::wstring>& namedResources,
        std::set<std::wstring>& requiredFilesRelativePaths)
    {
        if (namedResources.size() == 0)
        {
            return S_OK;
        }
        std::experimental::filesystem::v1::path resourcePriXmlDumpPath{ outputDirectoryPath };
        resourcePriXmlDumpPath /= resourcesPriXmlDumpFile;

        // Dump binary pri file to xml format.
        HRESULT dumpPriResult = MrmDumpPriFile(
            resourcesPriFilePath.c_str(),
            resourcesPriFilePath.c_str(),
            MrmDumpType_Detailed,
            resourcePriXmlDumpPath.c_str());
        RETURN_IF_FAILED(dumpPriResult);

        // Open dumped resources pri xml and find the default qualifiers
        ComPtr<IMsixElement> priDocumentElement;
        RETURN_IF_FAILED(CoCreateXmlFactory(MyAllocate, MyFree, const_cast<char*>(utf16_to_utf8(resourcePriXmlDumpPath).c_str()), &priDocumentElement));

        std::wstring defaultQualifiers;
        ComPtr<IMsixElementEnumerator> defaultQualifiersEnum;
        RETURN_IF_FAILED(priDocumentElement->GetElements(priDefaultQualifiersQuery.c_str(), &defaultQualifiersEnum));
        BOOL hasCurrentQualifier = FALSE;
        RETURN_IF_FAILED(defaultQualifiersEnum->GetHasCurrent(&hasCurrentQualifier));
        while (hasCurrentQualifier)
        {
            ComPtr<IMsixElement> qualifierElement;
            RETURN_IF_FAILED(defaultQualifiersEnum->GetCurrent(&qualifierElement));

            std::wstring scoreAsDefault{};
            RETURN_IF_FAILED(GetAttributeValueFromElement(qualifierElement.Get(), priScoreAsDefaultAttribute, scoreAsDefault));

            if (scoreAsDefault.compare(priDefaultQualifierScore) == 0)
            {
                RETURN_IF_FAILED(AppendToQualifierString(qualifierElement.Get(), defaultQualifiers));
            }

            RETURN_IF_FAILED(defaultQualifiersEnum->MoveNext(&hasCurrentQualifier));
        }

        // MrmCreateResourceIndexer is documented as taking package family name, but when dumping resource pris from existing packages and then recreating them,
        // it only matches when package name is used instead.
        MrmResourceIndexerHandle indexer;
        HRESULT createIndexerResult = MrmCreateResourceIndexer(
            packageName.c_str(),
            outputDirectoryPath.c_str(),
            MrmPlatformVersion_Windows10_0_0_5,
            defaultQualifiers.c_str(),
            &indexer);
        RETURN_IF_FAILED(createIndexerResult);

        // Iterate through each resource in the xml
        ComPtr<IMsixElementEnumerator> namedResourceEnum;
        RETURN_IF_FAILED(priDocumentElement->GetElements(namedResourceQuery.c_str(), &namedResourceEnum));
        BOOL hasCurrentNamedResource = FALSE;
        RETURN_IF_FAILED(namedResourceEnum->GetHasCurrent(&hasCurrentNamedResource));
        while (hasCurrentNamedResource)
        {
            ComPtr<IMsixElement> namedResourceElement;
            RETURN_IF_FAILED(namedResourceEnum->GetCurrent(&namedResourceElement));

            std::wstring resourceUri{};
            RETURN_IF_FAILED(GetAttributeValueFromElement(namedResourceElement.Get(), priUriAttribute, resourceUri));
#if _DEBUG
            std::wcout << L"Found resource uri " << resourceUri << std::endl;
#endif

            if (namedResources.find(resourceUri) != namedResources.end())
            {
                // Every resource has a set of candidate values that the named resource could resolve to depending on the system's language, scale, UI contrast, etc.
                // Iterate through each candidate and index each one into the new resources.pri file.
                ComPtr<IMsixElementEnumerator> candidateEnum;
                RETURN_IF_FAILED(namedResourceElement->GetElements(priCandidateQuery.c_str(), &candidateEnum));
                BOOL hasCurrentCandidateElement = FALSE;
                RETURN_IF_FAILED(candidateEnum->GetHasCurrent(&hasCurrentCandidateElement));
                while (hasCurrentCandidateElement)
                {
                    ComPtr<IMsixElement> candidateElement;
                    RETURN_IF_FAILED(candidateEnum->GetCurrent(&candidateElement));

                    std::wstring resourceQualifier{};
                    RETURN_IF_FAILED(GetResourceQualifierStringFromCandidate(candidateElement.Get(), resourceQualifier));

                    std::wstring resourceValue{};
                    RETURN_IF_FAILED(GetResourceValueStringFromCandidate(candidateElement.Get(), resourceValue));

                    std::wstring candidateType{};
                    RETURN_IF_FAILED(GetAttributeValueFromElement(candidateElement.Get(), priTypeAttribute, candidateType));
                    if (candidateType.compare(priPathCandidateTypeName) == 0)
                    {
                        // If the resource being resolved is a file path then add it to the requiredFilesRelativePaths set so that it ends up in the new package
                        requiredFilesRelativePaths.insert(resourceValue);

                        RETURN_IF_FAILED(MrmIndexFile(
                            indexer,
                            resourceUri.c_str(),
                            resourceValue.c_str(),
                            resourceQualifier.c_str()));
                    }
                    else if (candidateType.compare(priStringCandidateTypeName) == 0)
                    {
                        RETURN_IF_FAILED(MrmIndexString(
                            indexer,
                            resourceUri.c_str(),
                            resourceValue.c_str(),
                            resourceQualifier.c_str()));
                    }
                    else
                    {
                        return HRESULT_FROM_WIN32(ERROR_XML_PARSE_ERROR);
                    }

                    RETURN_IF_FAILED(candidateEnum->MoveNext(&hasCurrentCandidateElement));
                }
            }

            RETURN_IF_FAILED(namedResourceEnum->MoveNext(&hasCurrentNamedResource));
        }

        // Write the new resource file to the output directory. The file name will always be resources.pri
        HRESULT createPriResult = MrmCreateResourceFile(
            indexer,
            MrmPackagingModeStandaloneFile,
            MrmPackagingOptionsNone,
            outputDirectoryPath.c_str());
        RETURN_IF_FAILED(createPriResult);

        HRESULT destroyIndexer = ::MrmDestroyIndexerAndMessages(indexer);
        RETURN_IF_FAILED(destroyIndexer);

#if _DEBUG
        // Dump created file for manual verification.
        std::experimental::filesystem::v1::path tempOutputPriFilePath{ outputDirectoryPath };
        tempOutputPriFilePath /= resourcesPriFile;
        std::experimental::filesystem::v1::path tempOutputTrimmedXmlFilePath{ outputDirectoryPath };
        tempOutputTrimmedXmlFilePath /= L"resourcesPriTrimmed.xml";
        HRESULT dumpPriResult2 = MrmDumpPriFile(
            tempOutputPriFilePath.c_str(),
            tempOutputPriFilePath.c_str(),
            MrmDumpType_Detailed,
            tempOutputTrimmedXmlFilePath.c_str());
        RETURN_IF_FAILED(dumpPriResult2);
#else
        std::experimental::filesystem::remove(resourcePriXmlDumpPath);
#endif

        return S_OK;
    }

    bool IsDirectory(std::wstring directoryPath)
    {
        filesystem::path sourcePath(directoryPath);
        return filesystem::is_directory(sourcePath);
    }

    bool EndsWithSupportedExtension(std::wstring fileName)
    {
        // Supported image filenames for resource resolution are all at least one character, a period, and a three character image extension.
        if (fileName.length() < 5)
        {
            return false;
        }
        std::wstring extension = fileName.substr(fileName.length() - 4, 4);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension.compare(L".jpg") == 0 ||
            extension.compare(L".png") == 0 ||
            extension.compare(L".gif") == 0 ||
            extension.compare(L".dll") == 0 ||
            extension.compare(L".exe") == 0)
        {
            return true;
        }
        return false;
    }

    HRESULT FindPackageNameInManifestXml(
        IMsixElement* documentElement,
        _Inout_ std::wstring& packageName)
    {
        std::wstring nameAttributeValue{};
        ComPtr<IMsixElementEnumerator> identityElementEnum;
        RETURN_IF_FAILED(documentElement->GetElements(packageIdentityQuery.c_str(), &identityElementEnum));
        BOOL hasCurrentIdentityElement = FALSE;
        RETURN_IF_FAILED(identityElementEnum->GetHasCurrent(&hasCurrentIdentityElement));

        // Only one identity element can exist in an package manifest.
        ComPtr<IMsixElement> identityElement;
        RETURN_IF_FAILED(identityElementEnum->GetCurrent(&identityElement));

        // Return the package name
        RETURN_IF_FAILED(GetAttributeValueFromElement(identityElement.Get(), packageIdentityNameAttributeName, nameAttributeValue));

        if (nameAttributeValue.empty())
        {
            return HRESULT_FROM_WIN32(ERROR_XML_PARSE_ERROR);
        }
        packageName = nameAttributeValue;
        return S_OK;
    }

    HRESULT EvaluateStringAndAddToSetIfResource(
        std::wstring directoryPath,
        std::wstring stringToEvaluate,
        std::wstring packageName,
        std::set<std::wstring>& namedResources,
        std::set<std::wstring>& requiredFilesRelativePaths)
    {
        if (stringToEvaluate.empty())
        {
            return S_OK;
        }

        if ((stringToEvaluate.length() > msResourceScheme.length()) && CaseInsensitiveStartsWith(stringToEvaluate, msResourceScheme))
        {
            // Sample node from resource pri xml:
            // <NamedResource name = "ApplicationTitle" index = "0" uri = "ms-resource://ContosoApp/Resources/ApplicationTitle">
            std::wstring resourceWithoutMsResourcePrefix = stringToEvaluate.substr(msResourceScheme.length(), stringToEvaluate.length() - msResourceScheme.length());
            std::wstring resourceUri = msResourceScheme + L"//" + packageName + L"/" + msResourceResourceUriPath + L"/" + resourceWithoutMsResourcePrefix;
            namedResources.insert(resourceUri);
        }
        else if (EndsWithSupportedExtension(stringToEvaluate))
        {
            // This may resolve to many different files via the resource pri, while not existing itself.
            // e.g. for an attribute with value Square44x44Logo="Assets\AppListLogo.png", that file may not exist in the package
            // but "\Assets\AppListLogo.scale-100.png", "\Assets\AppListLogo.scale-200.png" will.
            std::filesystem::path filePath{ directoryPath };
            filePath /= stringToEvaluate;
            std::error_code existsCheckErrorCode{};
            bool fileExists = std::filesystem::exists(filePath, existsCheckErrorCode);
            if (existsCheckErrorCode.value() != 0)
            {
                return E_UNEXPECTED;
            }
            if (fileExists)
            {
                requiredFilesRelativePaths.insert(stringToEvaluate);
            }

            // Sample node from resource pri xml:
            //<NamedResource name="AppListLogo.png" index="191" uri="ms-resource://Microsoft.BingWeather/Files/Assets/AppListLogo.png">
            std::wstring stringAsUriPart = stringToEvaluate;
            std::replace(stringAsUriPart.begin(), stringAsUriPart.end(), '\\', '/');
            std::wstring resourceUri = msResourceScheme + L"//" + packageName + L"/" + msResourceFileUriPath + L"/" + stringAsUriPart;
            namedResources.insert(resourceUri);
        }
        return S_OK;
    }

    HRESULT FindResourceInfoInManifest(
        _In_ std::wstring sourceDirectoryPath,
        _Inout_ std::wstring& packageName,
        std::set<std::wstring>& namedResources,
        std::set<std::wstring>& requiredFilesRelativePaths)
    {
        std::experimental::filesystem::v1::path manifestFilePath{ sourceDirectoryPath };
        manifestFilePath /= manifestFile;

        ComPtr<IMsixElement> documentElement;
        RETURN_IF_FAILED(CoCreateXmlFactory(MyAllocate, MyFree, const_cast<char*>(utf16_to_utf8(manifestFilePath).c_str()), &documentElement));

        RETURN_IF_FAILED(FindPackageNameInManifestXml(documentElement.Get(), packageName));

        ComPtr<IMsixElementEnumerator> leafElementEnum;
        RETURN_IF_FAILED(documentElement->GetElements(leafElementQuery.c_str(), &leafElementEnum));
        BOOL hasCurrentLeafElement = FALSE;
        RETURN_IF_FAILED(leafElementEnum->GetHasCurrent(&hasCurrentLeafElement));
        while (hasCurrentLeafElement)
        {
            ComPtr<IMsixElement> element;
            RETURN_IF_FAILED(leafElementEnum->GetCurrent(&element));

            std::wstring textValue{};
            RETURN_IF_FAILED(GetTextValueFromElement(element.Get(), textValue));

            RETURN_IF_FAILED(EvaluateStringAndAddToSetIfResource(sourceDirectoryPath, textValue, packageName, namedResources, requiredFilesRelativePaths));

            RETURN_IF_FAILED(leafElementEnum->MoveNext(&hasCurrentLeafElement));
        }

        // Iterate through each element in the xml. Resources can be in attribute values or text values.
        ComPtr<IMsixElementEnumerator> childElementEnum;
        RETURN_IF_FAILED(documentElement->GetElements(childElementQuery.c_str(), &childElementEnum));
        BOOL hasCurrentChildElement = FALSE;
        RETURN_IF_FAILED(childElementEnum->GetHasCurrent(&hasCurrentChildElement));
        while (hasCurrentChildElement)
        {
            ComPtr<IMsixElement> element;
            RETURN_IF_FAILED(childElementEnum->GetCurrent(&element));

            ComPtr<IMsixAttributesEnumerator> attributesEnum;
            RETURN_IF_FAILED(element->GetAttributes(&attributesEnum));

            BOOL hasCurrentAttributeElement = FALSE;
            RETURN_IF_FAILED(attributesEnum->GetHasCurrent(&hasCurrentAttributeElement));
            while (hasCurrentAttributeElement)
            {
                std::wstring attributeName;
                Text<wchar_t> attributeNameText;
                RETURN_IF_FAILED(attributesEnum->GetCurrent(&attributeNameText));
                if (attributeNameText.Get() != nullptr)
                {
                    attributeName = attributeNameText.Get();
                }

                std::wstring attributeValue{};
                RETURN_IF_FAILED(GetAttributeValueFromElement(element.Get(), attributeName, attributeValue));

                    RETURN_IF_FAILED(EvaluateStringAndAddToSetIfResource(sourceDirectoryPath, attributeValue, packageName, namedResources, requiredFilesRelativePaths));

                RETURN_IF_FAILED(attributesEnum->MoveNext(&hasCurrentAttributeElement));
            }

            RETURN_IF_FAILED(childElementEnum->MoveNext(&hasCurrentChildElement));
        }

        return S_OK;
    }

    HRESULT CopyRequiredFiles(std::wstring originalDirectory, std::wstring outputDirectory, std::set<std::wstring>& requiredFilesRelativePaths)
    {
        try
        {
            for (std::wstring relativeFilePath : requiredFilesRelativePaths)
            {
                std::filesystem::path originalFilePath{ originalDirectory };
                originalFilePath /= relativeFilePath;
                std::filesystem::path newFilePath{ outputDirectory };
                newFilePath /= relativeFilePath;

#if _DEBUG
                std::wcout << L"Copying file " << originalFilePath << std::endl;
#endif

                std::filesystem::path dirPath = newFilePath.parent_path();
                std::filesystem::create_directories(dirPath);
                std::filesystem::copy_file(originalFilePath, newFilePath);
            }
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return E_UNEXPECTED;
        }

        return S_OK;
    }

    HRESULT CreateKozaniResourcePriFromPackage(
        _In_ std::wstring sourceDirectoryPath,
        _In_ std::wstring outputDirectoryPath,
        std::set<std::wstring>& requiredFilesRelativePaths)
    {
        std::experimental::filesystem::v1::path manifestFilePath{ sourceDirectoryPath };
        manifestFilePath /= manifestFile;

        // Manifest is always required for any package.
        requiredFilesRelativePaths.insert(manifestFileName);

        //Get all required resource uris and all required filenames from the appxmanifest.xml
        std::wstring packageName = L"";
        std::set<std::wstring> namedResourcesFromManifest;
        RETURN_IF_FAILED(FindResourceInfoInManifest(sourceDirectoryPath, packageName, namedResourcesFromManifest, requiredFilesRelativePaths));

        // Get the list of required files (e.g. tile images) to fulfill manifest resource resolution.
        std::experimental::filesystem::v1::path resourcesPriFilePath{ sourceDirectoryPath };
        resourcesPriFilePath /= resourcesPriFile;
        RETURN_IF_FAILED(CreateKozaniResourcePriWithNamedResources(
            resourcesPriFilePath,
            outputDirectoryPath,
            packageName,
            namedResourcesFromManifest,
            requiredFilesRelativePaths));

        return S_OK;
    }

    HRESULT CreateKozaniPackage(
        _In_ std::wstring sourceDirectoryPath,
        _In_ std::wstring outputDirectoryPath)
    {
        if (!IsDirectory(sourceDirectoryPath) ||
            !IsDirectory(outputDirectoryPath) ||
            (sourceDirectoryPath.compare(outputDirectoryPath) == 0))
        {
            return E_INVALIDARG;
        }

        std::set<std::wstring> requiredFilesRelativePaths;
        RETURN_IF_FAILED(CreateKozaniResourcePriFromPackage(sourceDirectoryPath, outputDirectoryPath, requiredFilesRelativePaths));

        RETURN_IF_FAILED(CopyRequiredFiles(sourceDirectoryPath, outputDirectoryPath, requiredFilesRelativePaths));

        return S_OK;
    }
}