#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

#else
#include <unistd.h>
#endif


#include <string>
#include <filesystem>
#include <memory>

#include "platform.h"
#include "common.h"
#include "cd.h"
#include "xa.h"
#include "cdreader.h"
#include "xml.h"

#include <time.h>


namespace param {

    std::filesystem::path isoFile;
    std::filesystem::path outPath;
    std::filesystem::path xmlFile;

    int		printOnly=false;

}

template<size_t N>
void PrintId(char (&id)[N])
{
	std::string_view view;

	const std::string_view id_view(id, N);
	const size_t last_char = id_view.find_last_not_of(' ');
	if (last_char != std::string_view::npos)
	{
		view = id_view.substr(0, last_char+1);
	}

	if (!view.empty())
	{
		printf("%.*s", static_cast<int>(view.length()), view.data());
	}
	printf("\n");
}

void PrintDate(const cd::ISO_LONG_DATESTAMP& date) {
    printf("%s\n", LongDateToString(date).c_str());
}

template<size_t N>
static std::string CleanDescElement(char (&id)[N])
{
	std::string result;

	const std::string_view view(id, N);
	const size_t last_char = view.find_last_not_of(' ');
	if (last_char != std::string_view::npos)
	{
		result = view.substr(0, last_char+1);
	}

    return result;
}

std::string CleanIdentifier(std::string_view id)
{
	std::string result(id.substr(0, id.find_last_of(';')));
	return result;
}

void prepareRIFFHeader(cd::RIFF_HEADER* header, int dataSize) {
	memcpy(header->chunkID,"RIFF",4);
	header->chunkSize = 36 + dataSize;
	memcpy(header->format, "WAVE", 4);

	memcpy(header->subchunk1ID, "fmt ", 4);
	header->subchunk1Size = 16;
	header->audioFormat = 1;
	header->numChannels = 2;
	header->sampleRate = 44100;
	header->bitsPerSample = 16;
	header->byteRate = (header->sampleRate * header->numChannels * header->bitsPerSample) / 8;
	header->blockAlign = (header->numChannels * header->bitsPerSample) / 8;

	memcpy(header->subchunk2ID, "data", 4);
	header->subchunk2Size = dataSize;
}
std::unique_ptr<cd::ISO_LICENSE> ReadLicense(cd::IsoReader& reader) {
	auto license = std::make_unique<cd::ISO_LICENSE>();

	reader.SeekToSector(0);
	reader.ReadBytesXA(license->data, sizeof(license->data));

	return license;
}

void SaveLicense(const cd::ISO_LICENSE& license) {
    const std::filesystem::path outputPath = param::outPath / "license_data.dat";

	FILE* outFile = OpenFile(outputPath, "wb");

    if (outFile == NULL) {
		printf("ERROR: Cannot create license file %" PRFILESYSTEM_PATH "...", outputPath.lexically_normal().c_str());
        return;
    }

    fwrite(license.data, 1, sizeof(license.data), outFile);
    fclose(outFile);
}

static void WriteOptionalXMLAttribs(tinyxml2::XMLElement* element, const cd::IsoDirEntries::Entry& entry, const bool directory, const bool XA)
{
	if (element == nullptr)
	{
		return;
	}

	element->SetAttribute(xml::attrib::GMT_OFFSET, entry.entry.entryDate.GMToffs);

	// Don't output XA attributes on directories yet
	// TODO: Might need something to allow those to propagate upwards with a file/directory distinction
	if (!directory)
	{
		// xa_attrib only makes sense on XA files
		if (XA)
		{
			element->SetAttribute(xml::attrib::XA_ATTRIBUTES, (entry.extData.attributes & cdxa::XA_ATTRIBUTES_MASK) >> 8);
		}
		element->SetAttribute(xml::attrib::XA_PERMISSIONS, entry.extData.attributes & cdxa::XA_PERMISSIONS_MASK);

		element->SetAttribute(xml::attrib::XA_GID, entry.extData.ownergroupid);
		element->SetAttribute(xml::attrib::XA_UID, entry.extData.owneruserid);
	}
}

void ParseDirectories(cd::IsoReader& reader, int offs, tinyxml2::XMLElement* element, int sectors,
	const std::filesystem::path& srcPath, const std::filesystem::path& xmlPath) {

    cd::IsoDirEntries dirEntries;

    dirEntries.ReadDirEntries(&reader, offs, sectors);
    dirEntries.SortByLBA();

	// Iterate through all entries, skipping the first two (. and ..)
	if (dirEntries.dirEntryList.size() < 2)
	{
		// Empty directory
		return;
	}

	// Write out attributes of the current directory
	WriteOptionalXMLAttribs(element, dirEntries.dirEntryList.front(), true, false);

    for(auto it = std::next(dirEntries.dirEntryList.begin(), 2); it != dirEntries.dirEntryList.end(); ++it)
	{
		const auto& entry = *it;
		const std::filesystem::path outputPath = srcPath / CleanIdentifier(entry.identifier);

		tinyxml2::XMLElement* newelement = nullptr;
        if (entry.entry.flags & 0x2)
		{
            newelement = element->InsertNewChildElement("dir");
            newelement->SetAttribute(xml::attrib::ENTRY_NAME, entry.identifier.c_str());

			// TODO: Dump 'source' only on the first occurence of this directory
			newelement->SetAttribute(xml::attrib::ENTRY_SOURCE, outputPath.lexically_proximate(xmlPath).generic_u8string().c_str());

            ParseDirectories(reader, entry.entry.entryOffs.lsb, newelement, entry.entry.entrySize.lsb/2048, outputPath, xmlPath);

        }
		else
		{
			printf("   Extracting %s...\n%" PRFILESYSTEM_PATH "\n", entry.identifier.c_str(), outputPath.lexically_normal().c_str());

            newelement = element->InsertNewChildElement("file");
            newelement->SetAttribute(xml::attrib::ENTRY_NAME, CleanIdentifier(entry.identifier).c_str());
            newelement->SetAttribute(xml::attrib::ENTRY_SOURCE, outputPath.lexically_proximate(xmlPath).generic_u8string().c_str());		

			const unsigned short xa_attr = (entry.extData.attributes & cdxa::XA_ATTRIBUTES_MASK) >> 8;

			char type = -1;

			// we try to guess the file type. Usually, the xa_attr should tell this, but there are many games
			// that do not follow the standard, and sometime leave some or all the attributes unset.

			if (xa_attr & 0x40) {
				// if the cddata flag is set, we assume this is the case
				type = 1;
			} else if ( (xa_attr & 0x08) && !(xa_attr & 0x10) ){
				// if the mode 2 form 1 flag is set, and form 2 is not, we assume this is a regular file.
				type = 0;
			} else if ( (xa_attr & 0x10) && !(xa_attr & 0x08) ) {
				// if the mode 2 form 2 flag is set, and form 1 is not, we assume this is a pure audio xa file.
				type = 2;
			}  else {
				// here all flags are set to the same value. From what I could see until now, when both flags are the same,
				// this is interpreted in the following two ways, which both lead us to choose str/xa type.
				// 1. Both values are 1, which means there is an indication by the mode 2 form 2 flag that the data is not
				//    regular mode 2 form 1 data (i.e., it is either mixed or just xa). 
				// 2. Both values are 0. The fact that the mode 2 form 2 flag is 0 simply means that the data might not
				//    be *pure* mode 2 form 2 data (i.e., xa), so, we do not conclude it is regular mode 2 form 1 data.
				//    We thus give priority to the mode 2 form 1 flag, which is also zero,
				//	  and conclude that the data is not regular mode 2 form 1 data, and thus can be either mode 2 form 2 or mixed.

				// Remark: Some games (Legend of Mana), use a very strange STR+XA format that is stored in plain mode 2 form 1.
				// This is properly marked in the xa_attr, and there is nothing wrong in extracting them as data.
				type = 2;
			}

			if (type == 2) {

				// Extract XA or STR file.
				// For both XA and STR files, we need to extract the data 2336 bytes per sector.
				// When rebuilding the bin using mkpsxiso, either we mark the file with str or with xa
				// the source file will anyway be stored on our hard drive in raw form.
				// Here we mark the file as str, so that each sector will have the correct error codes regenerated by mkpsxiso,
				// depending on the value of the sub-header (video or audio data).
				newelement->SetAttribute(xml::attrib::ENTRY_TYPE, "mixed");

				// this is the data to be read 2336 bytes per sector, both if the file is an STR or XA,
				// because the STR contains audio.
				size_t sectorsToRead = (entry.entry.entrySize.lsb+2047)/2048;

				int bytesLeft = 2336*sectorsToRead;

				reader.SeekToSector(entry.entry.entryOffs.lsb);

				FILE* outFile = OpenFile(outputPath, "wb");

				if (outFile == NULL) {
					printf("ERROR: Cannot create file %" PRFILESYSTEM_PATH "...", outputPath.lexically_normal().c_str());
					return;
				}

				// Copy loop
				while(bytesLeft > 0) {

					u_char copyBuff[2336];

					int bytesToRead = bytesLeft;

					if (bytesToRead > 2336)
						bytesToRead = 2336;

					reader.ReadBytesXA(copyBuff, 2336);

					fwrite(copyBuff, 1, 2336, outFile);

					bytesLeft -= bytesToRead;

				}

				fclose(outFile);

			}
			else if (type == 1) {

				// Extract CDDA file
				newelement->SetAttribute(xml::attrib::ENTRY_TYPE, "da");

				int result = reader.SeekToSector(entry.entry.entryOffs.lsb);

				if (result) {
					printf("WARNING: The CDDA file %" PRFILESYSTEM_PATH " is out of the iso file bounds.\n", outputPath.lexically_normal().c_str());
					printf("This usually means that the game has audio tracks, and they are on separate files.\n");
					printf("As DUMPSXISO does not support dumping from a cue file, you should use an iso file containing all tracks.\n\n");
					printf("DUMPSXISO will write the file as a dummy (silent) cdda file.\n");
					printf("This is generally fine, when the real CDDA file is also a dummy file.\n");
					printf("If it is not dummy, you WILL lose this audio data in the rebuilt iso.\n");
				}

				FILE* outFile = OpenFile(outputPath, "wb");

				if (outFile == NULL) {
					printf("ERROR: Cannot create file %" PRFILESYSTEM_PATH "...", outputPath.lexically_normal().c_str());
					return;
				}

				size_t sectorsToRead = (entry.entry.entrySize.lsb + 2047) / 2048;

				int cddaSize = 2352 * sectorsToRead;
				int bytesLeft = cddaSize;

				cd::RIFF_HEADER riffHeader;

				prepareRIFFHeader(&riffHeader, cddaSize);
				fwrite((void*)&riffHeader, 1, sizeof(cd::RIFF_HEADER), outFile);


				while (bytesLeft > 0) {

					u_char copyBuff[2352];
					memset(copyBuff, 0, 2352);

					int bytesToRead = bytesLeft;

					if (bytesToRead > 2352)
						bytesToRead = 2352;

					if (!result)
						reader.ReadBytesDA(copyBuff, bytesToRead);
					
					fwrite(copyBuff, 1, bytesToRead, outFile);

					bytesLeft -= bytesToRead;

				}

				fclose(outFile);

			} else {

				// Extract regular file
				newelement->SetAttribute(xml::attrib::ENTRY_TYPE, "data");

				reader.SeekToSector(entry.entry.entryOffs.lsb);

				FILE* outFile = OpenFile(outputPath, "wb");

				if (outFile == NULL) {
					printf("ERROR: Cannot create file %" PRFILESYSTEM_PATH "...", outputPath.lexically_normal().c_str());
					return;
				}

				int bytesLeft = entry.entry.entrySize.lsb;
				while(bytesLeft > 0) {

					u_char copyBuff[2048];
					int bytesToRead = bytesLeft;

					if (bytesToRead > 2048)
						bytesToRead = 2048;

					reader.ReadBytes(copyBuff, bytesToRead);
					fwrite(copyBuff, 1, bytesToRead, outFile);

					bytesLeft -= bytesToRead;

				}

				fclose(outFile);

			}

			WriteOptionalXMLAttribs(newelement, entry, false, type == 2);
        }
		UpdateTimestamps(outputPath, entry.entry.entryDate);
    }
}

void ParseISO(cd::IsoReader& reader) {

    cd::ISO_DESCRIPTOR descriptor;
	auto license = ReadLicense(reader);

    reader.SeekToSector(16);
    reader.ReadBytes(&descriptor, 2048);


    printf("ISO decriptor:\n\n");

    printf("   System ID      : ");
    PrintId(descriptor.systemID);
    printf("   Volume ID      : ");
    PrintId(descriptor.volumeID);
    printf("   Volume Set ID  : ");
    PrintId(descriptor.volumeSetIdentifier);
    printf("   Publisher ID   : ");
    PrintId(descriptor.publisherIdentifier);
    printf("   Data Prep. ID  : ");
    PrintId(descriptor.dataPreparerIdentifier);
    printf("   Application ID : ");
    PrintId(descriptor.applicationIdentifier);
    printf("\n");

    printf("   Volume Create Date : ");
    PrintDate(descriptor.volumeCreateDate);
    printf("   Volume Modify Date : ");
    PrintDate(descriptor.volumeModifyDate);
    printf("   Volume Expire Date : ");
    PrintDate(descriptor.volumeExpiryDate);
    printf("\n");

    cd::IsoPathTable pathTable;

    size_t numEntries = pathTable.ReadPathTable(&reader, descriptor.pathTable1Offs);

    if (numEntries == 0) {
        printf("   No files to find.\n");
        return;
    }

	// Prepare output directories
	for(size_t i=0; i<numEntries; i++)
	{
		const std::filesystem::path dirPath = param::outPath / pathTable.GetFullDirPath(i);

		std::error_code ec;
		std::filesystem::create_directories(dirPath, ec);
	}

    printf("ISO contents:\n\n");

    tinyxml2::XMLDocument xmldoc;

    if (!param::xmlFile.empty()) {

		tinyxml2::XMLElement *baseElement = static_cast<tinyxml2::XMLElement*>(xmldoc.InsertFirstChild(xmldoc.NewElement(xml::elem::ISO_PROJECT)));
		baseElement->SetAttribute(xml::attrib::IMAGE_NAME, "mkpsxiso.bin");
		baseElement->SetAttribute(xml::attrib::CUE_SHEET, "mkpsxiso.cue");

		tinyxml2::XMLElement *trackElement = baseElement->InsertNewChildElement(xml::elem::TRACK);
		trackElement->SetAttribute(xml::attrib::TRACK_TYPE, "data");

		{
			tinyxml2::XMLElement *newElement = trackElement->InsertNewChildElement(xml::elem::IDENTIFIERS);
			auto setAttributeIfNotEmpty = [newElement](const char* name, const std::string& value)
			{
				if (!value.empty())
				{
					newElement->SetAttribute(name, value.c_str());
				}
			};

			setAttributeIfNotEmpty(xml::attrib::SYSTEM_ID, CleanDescElement(descriptor.systemID));
			setAttributeIfNotEmpty(xml::attrib::APPLICATION, CleanDescElement(descriptor.applicationIdentifier));
			setAttributeIfNotEmpty(xml::attrib::VOLUME_ID, CleanDescElement(descriptor.volumeID));
			setAttributeIfNotEmpty(xml::attrib::VOLUME_SET, CleanDescElement(descriptor.volumeSetIdentifier));
			setAttributeIfNotEmpty(xml::attrib::PUBLISHER, CleanDescElement(descriptor.publisherIdentifier));
			setAttributeIfNotEmpty(xml::attrib::DATA_PREPARER, CleanDescElement(descriptor.dataPreparerIdentifier));
			newElement->SetAttribute(xml::attrib::CREATION_DATE, LongDateToString(descriptor.volumeCreateDate).c_str());
		}

		{
			tinyxml2::XMLElement *newElement = trackElement->InsertNewChildElement(xml::elem::LICENSE);
			newElement->SetAttribute(xml::attrib::LICENSE_FILE,
				(param::outPath / "license_data.dat").lexically_proximate(param::xmlFile.parent_path()).generic_u8string().c_str());
		}

		{
			tinyxml2::XMLElement *newElement = trackElement->InsertNewChildElement(xml::elem::DIRECTORY_TREE);

			ParseDirectories(reader,
							descriptor.rootDirRecord.entryOffs.lsb,
							newElement,
							descriptor.rootDirRecord.entrySize.lsb/2048,
							param::outPath, param::xmlFile.parent_path());
		}

		if (FILE* file = OpenFile(param::xmlFile, "w"); file != nullptr)
		{
			xmldoc.SaveFile(file);
			fclose(file);
		}

    } else {

    	ParseDirectories(reader,
						descriptor.rootDirRecord.entryOffs.lsb,
						xmldoc.NewElement(""), // Dummy
						descriptor.rootDirRecord.entrySize.lsb/2048,
						param::outPath, std::filesystem::path());

    }

    //Save license file anyway.
    SaveLicense(*license);
}

int Main(int argc, char *argv[])
{
    printf("DUMPSXISO " VERSION " - PlayStation ISO dumping tool\n");
    printf("2017 Meido-Tek Productions (Lameguy64)\n");
    printf("2020 Phoenix (SadNES cITy)\n");
    printf("2021 Silent and Chromaryu\n\n");

	if (argc == 1) {

		printf("Usage:\n\n");
		printf("   dumpsxiso <isofile> [-x <path>]\n\n");
		printf("   <isofile>   - File name of ISO file (supports any 2352 byte/sector images).\n");
		printf("   [-x <path>] - Specified destination directory of extracted files.\n");
		printf("   [-s <path>] - Outputs an MKPSXISO compatible XML script for later rebuilding.\n");

		return(EXIT_SUCCESS);

	}


    for(int i=1; i<argc; i++) {

		// Is it a switch?
        if (argv[i][0] == '-') {

			// Directory path
            if (strcmp("x", &argv[i][1]) == 0) {

                param::outPath = std::filesystem::u8path(argv[i+1]).lexically_normal();

                i++;

            } else if (strcmp("s", &argv[i][1]) == 0) {

                param::xmlFile = std::filesystem::u8path(argv[i+1]);
                i++;

            } else if (strcmp("p", &argv[i][1]) == 0) {

            	param::printOnly = true;

            } else {

            	printf("Unknown parameter: %s\n", argv[i]);
            	return(EXIT_FAILURE);

            }

        } else {

			if (param::isoFile.empty()) {

				param::isoFile = std::filesystem::u8path(argv[i]);

			} else {

				printf("Only one iso file is supported.\n");
				return(EXIT_FAILURE);

			}

        }

    }

	if (param::isoFile.empty()) {

		printf("No iso file specified.\n");
		return(EXIT_FAILURE);

	}


	cd::IsoReader reader;

	if (!reader.Open(param::isoFile)) {

		printf("ERROR: Cannot open file %" PRFILESYSTEM_PATH "...\n", param::isoFile.lexically_normal().c_str());
		return(EXIT_FAILURE);

	}

	if (!param::outPath.empty())
	{
		printf("Output directory : %" PRFILESYSTEM_PATH "\n", param::outPath.lexically_normal().c_str());
	}

    ParseISO(reader);

    reader.Close();

    exit(EXIT_SUCCESS);

}
