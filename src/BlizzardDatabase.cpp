#include "BlizzardDatabase.h"
#include <cassert>
#include <stdexcept>

namespace BlizzardDatabaseLib
{
    BlizzardDatabase::BlizzardDatabase(const std::string& databaseDefinitionDirectory, const Structures::Build& build, bool preferDb2)
    : _databaseDefinitionFilesLocation(databaseDefinitionDirectory)
    , _build(build)
    , _preferDb2(preferDb2)
    {
        _loadedTables = std::map<std::string, std::shared_ptr<BlizzardDatabaseTable>>();
        _blizzardTableReaderFactory = Reader::BlizzardTableReaderFactory();
        _table_definitions = std::map<std::string, Structures::VersionDefinition>();
    }

    // must use as a ref, copies not allowed
    BlizzardDatabaseTable& BlizzardDatabase::LoadTable(const std::string& tableName,
       std::function<std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>(std::string const&)> file_callback)
    {
        if (_loadedTables.contains(tableName))
        {
            std::cout << "Table Already Loaded" << std::endl;
            return *_loadedTables[tableName];
        }

        // get table definition
        auto absoluteFilePathOfDatabaseTableDefinition =  std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = databaseDefinition.For(_build, tableDefinition); // unclean table definition.  pruned one from : WDBCTableReader::RecordDefinition()
        tableDefinition.tableName = tableName;

        // HACKFIX START -- We should probably be doing proper detection to see if a .db2 file exists first, if not fallback to .dbc
        auto fileName = "DBFilesClient\\" + tableName + ".dbc";
        const Structures::Build& dbcCutoffBuild = Structures::Build("7.0.3.21287"); // First build with no more DBC files at all.
        if (_preferDb2 || _build > dbcCutoffBuild) {
            fileName = "DBFilesClient\\" + tableName + ".db2";
        }
        
        auto fileStream = file_callback(fileName);
        // HACKFIX END

        auto streamReader = std::make_shared<Stream::StreamReader>(fileStream);
        auto fileFormatIdentifier = streamReader->ReadString(4);

        if (!tableFound &&
            (fileFormatIdentifier == "WDC5" || fileFormatIdentifier == "WDC4") &&
            streamReader->Length() >= 4 + sizeof(Structures::WDC5Header))
        {
            auto header = streamReader->Read<Structures::WDC5Header>();
            tableFound = databaseDefinition.ForLayoutHash(header.LayoutHash, tableDefinition);
            tableDefinition.tableName = tableName;
            streamReader->Jump(4);
        }

        if (!tableFound)
            throw std::runtime_error("Database definition build/layout not found for " + tableName);

        tableDefinition.useGlobalStringOffsets = _preferDb2;

        auto tableReader = _blizzardTableReaderFactory.For(streamReader, tableDefinition, fileFormatIdentifier);

        auto constructedTable = std::make_shared<BlizzardDatabaseTable>(tableReader, tableName);
        constructedTable->LoadTableStructure();

        _table_definitions.insert_or_assign(tableName, tableDefinition);
        _loadedTables.emplace(tableName, constructedTable);

        return *_loadedTables[tableName];
    }

    bool BlizzardDatabase::SaveTable(const std::string& outputDirectory, const std::string& tableName, std::vector<Structures::BlizzardDatabaseRow>& rows)
    {  
        auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = databaseDefinition.For(_build, tableDefinition);

        if (!tableFound)
            throw std::runtime_error("Database definition version not found for " + tableName);

        auto filePath = std::filesystem::path(outputDirectory) / (tableName + ".dbc");
        auto outputStream = std::ofstream(filePath, std::ios::out | std::ios::binary);

        auto fileWriter = Writer::WDBCTableWriter(outputStream,  tableDefinition);

        // TODO : also need to update the table in memory for the change

        return fileWriter.Write(rows);
    }

    void BlizzardDatabase::UnloadTable(const std::string& tableName)
    {
        if (!_loadedTables.contains(tableName))
        {
            std::cout << "Table Not Loaded" << std::endl;
            return;
        }

        auto& table = _loadedTables.at(tableName);

        table.reset();

        _loadedTables.erase(tableName);
    }

    Structures::VersionDefinition& BlizzardDatabase::TableDefinition(const std::string& tableName)
    {
      if (_table_definitions.contains(tableName))
      {
        return _table_definitions[tableName];
      }

      auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
        (tableName + ".dbd");

      auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
      auto tableVersionDefinition = Structures::VersionDefinition();

      auto tableFound = databaseDefinition.For(_build, tableVersionDefinition); // this initializes definition for the version

      if (!tableFound)
      {
        throw std::runtime_error("Database definition version not found for " + tableName);
      }

      tableVersionDefinition.tableName = tableName;

      _table_definitions.emplace(tableName, tableVersionDefinition);

      return _table_definitions.at(tableName);
    }

    Structures::BlizzardDatabaseRowDefinition& BlizzardDatabase::TableRecordDefinition(const std::string& tableName)
    {
      return TableDefinition(tableName).RowDefinition;
    }
}
