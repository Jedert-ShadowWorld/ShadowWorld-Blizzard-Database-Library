#include "BlizzardDatabase.h"
#include <cassert>
#include <stdexcept>

namespace BlizzardDatabaseLib
{
    namespace
    {
        bool ResolveTableDefinition(DatabaseDefinition& databaseDefinition,
                                    const Structures::Build& requestedBuild,
                                    Structures::VersionDefinition& tableDefinition)
        {
            if (databaseDefinition.For(requestedBuild, tableDefinition))
                return true;

            // The upstream DBD set used by Noggit does not list every 9.2.7 client
            // build explicitly. 9.2.7.45745 uses the final Shadowlands WDC3 layouts
            // represented by the latest 9.2.5 definitions in this definition set.
            // Never continue with an empty VersionDefinition: WDC3 can appear to load
            // its header/record count and then crash when records are materialized.
            if (requestedBuild.expansion() == 9 && requestedBuild.major() == 2 && requestedBuild.minor() == 7)
            {
                Structures::VersionDefinition shadowlandsDefinition;
                const Structures::Build schemaBuild("9.2.5.43412");
                if (databaseDefinition.For(schemaBuild, shadowlandsDefinition))
                {
                    tableDefinition = shadowlandsDefinition;
                    std::cout << "Using Shadowlands 9.2.5 DB2 schema for 9.2.7.45745" << std::endl;
                    return true;
                }
            }

            return false;
        }
    }

    BlizzardDatabase::BlizzardDatabase(const std::string& databaseDefinitionDirectory, const Structures::Build& build)
    : _databaseDefinitionFilesLocation(databaseDefinitionDirectory)
    , _build(build)
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

        auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = ResolveTableDefinition(databaseDefinition, _build, tableDefinition);
        tableDefinition.tableName = tableName;

        if (!tableFound)
            throw std::runtime_error("No DB2 schema definition for table " + tableName + " and requested build");

        auto fileName = "DBFilesClient\\" + tableName + ".dbc";
        const Structures::Build& dbcCutoffBuild = Structures::Build("7.0.3.21287");
        if (_build > dbcCutoffBuild)
            fileName = "DBFilesClient\\" + tableName + ".db2";

        auto fileStream = file_callback(fileName);
        auto streamReader = std::make_shared<Stream::StreamReader>(fileStream);
        auto fileFormatIdentifier = streamReader->ReadString(4);

        auto tableReader = _blizzardTableReaderFactory.For(streamReader, tableDefinition, fileFormatIdentifier);
        auto constructedTable = std::make_shared<BlizzardDatabaseTable>(tableReader, tableName);
        constructedTable->LoadTableStructure();

        _loadedTables.emplace(tableName, constructedTable);
        return *_loadedTables[tableName];
    }

    bool BlizzardDatabase::SaveTable(const std::string& outputDirectory, const std::string& tableName, std::vector<Structures::BlizzardDatabaseRow>& rows)
    {
        auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = ResolveTableDefinition(databaseDefinition, _build, tableDefinition);

        if (!tableFound)
            throw std::runtime_error("No database schema definition for table " + tableName + " and requested build");

        auto filePath = std::filesystem::path(outputDirectory) / (tableName + ".dbc");
        auto outputStream = std::ofstream(filePath, std::ios::out | std::ios::binary);
        auto fileWriter = Writer::WDBCTableWriter(outputStream, tableDefinition);
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
        return _table_definitions[tableName];

      auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
        (tableName + ".dbd");

      auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
      auto tableVersionDefinition = Structures::VersionDefinition();
      auto tableFound = ResolveTableDefinition(databaseDefinition, _build, tableVersionDefinition);

      if (!tableFound)
        throw std::runtime_error("No database schema definition for table " + tableName + " and requested build");

      tableVersionDefinition.tableName = tableName;
      _table_definitions.emplace(tableName, tableVersionDefinition);
      return _table_definitions[tableName];
    }

    Structures::BlizzardDatabaseRowDefinition& BlizzardDatabase::TableRecordDefinition(const std::string& tableName)
    {
      return TableDefinition(tableName).RowDefinition;
    }
}
