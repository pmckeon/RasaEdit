using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.Extensions.Options;
using Rasa.Configuration;
using Rasa.Configuration.ConnectionStrings;
using Rasa.Configuration.ContextSetup;
using Rasa.Context.World;
using Rasa.Managers;
using Rasa.Repositories.World;
using Rasa.Services.DbContext;

namespace Harness
{
    // Rasa.NET's mission content checks as MissionContentCatalog runs them at start-up (enabled definitions, one per mission;
    // loader; native object packages; validator), on the World database given (path without ".db").
    internal static class Program
    {
        private static int Main(string[] args)
        {
            var connection = new DatabaseConnectionConfiguration { Database = args[0] };
            var options = Options.Create(new DatabaseConfiguration { Provider = "Sqlite", Auth = connection, Char = connection, World = connection });
            using var context = new SqliteWorldContext(options, new SqliteDbContextConfigurationService(new SqliteConnectionStringFactory()),
                new SqliteDbContextPropertyModifier());
            var unit = new Unit(context);
            try
            {
                var migrated = (IMigratedMissionContentRepository)unit.MissionContent;
                var enabled = migrated.GetEnabledDefinitions();
                if (enabled.Count == 0) throw new InvalidOperationException("No enabled mission definitions.");
                if (enabled.GroupBy(e => e.MissionId).Any(g => g.Count() != 1))
                    throw new InvalidOperationException("Mission migrations enabled more than one definition for a mission.");
                var selected = enabled.ToDictionary(e => e.MissionId, e => e.ContentRevision);
                var snapshot = new MissionContentLoader().Load(unit.MissionContent, selected);
                var packages = MissionContentValidator.GetNativeObjectPackages(snapshot, unit);
                var report = new MissionContentValidator().Validate(snapshot, unit, packages);
                Console.WriteLine($"server check: {selected.Count} enabled, {report.Diagnostics.Count} diagnostics, readiness {(report.BlocksReadiness ? "BLOCKED" : "ok")}");
                foreach (var d in report.Diagnostics)
                    Console.WriteLine($"  mission {d.MissionId} {d.ContentRevision} o{N(d.ObjectiveId)} t{N(d.TransitionId)} g{N(d.TriggerId)} a{N(d.ActionId)} s{N(d.ScenarioId)} p{N(d.StepId)} {d.Code}: {d.Message}");
                return 0;
            }
            catch (Exception e)
            {
                Console.WriteLine("server check: START-UP FAILS: " + e.GetType().Name + ": " + e.Message);
                return 1;
            }
        }

        private static string N(uint? v) => v.HasValue ? v.Value.ToString() : "-1";

        // As Rasa.Test's RepositoryBackedWorldUnitOfWork (BootcampStartupValidationTests).
        private sealed class Unit : IWorldUnitOfWork
        {
            public Unit(SqliteWorldContext context)
            {
                Equipment = new EquipmentRepository(context);
                Creatures = new CreatureRepository(context);
                EntityClasses = new EntityClassRepository(context);
                MapInfos = new MapInfoRepository(context);
                MissionContent = new MissionContentRepository(context);
                OrdinaryLoot = new OrdinaryLootRepository(context);
                NpcPackages = new NpcPackageRepository(context);
                Teleporters = new TeleporterRepository(context);
            }
            public IActionRepository Actions { get; }
            public IEquipmentRepository Equipment { get; }
            public ICreatureRepository Creatures { get; }
            public IEntityClassRepository EntityClasses { get; }
            public IFootlockerRepository Footlockers { get; }
            public ILogosRepository Logoses { get; }
            public IMapInfoRepository MapInfos { get; }
            public IMapLinkRepository MapLinks { get; }
            public IKraftwerksRepository Kraftwerks { get; }
            public IMapRegionRepository MapRegions { get; }
            public IMapMarkerRepository MapMarkers { get; }
            public IRecipeRepository Recipes { get; }
            public INpcMissionRepository NpcMissions { get; }
            public INpcMissionRewardRepository NpcMissionRewards { get; }
            public IMissionContentRepository MissionContent { get; }
            public IOrdinaryLootRepository OrdinaryLoot { get; }
            public INpcPackageRepository NpcPackages { get; }
            public IPlayerRandomNameRepository RandomNames { get; }
            public ISpawnpoolRepository Spawnpools { get; }
            public IMapEmitterRepository MapEmitters { get; }
            public ISpawnPoolArrivalRepository SpawnPoolArrivals { get; }
            public ITeleporterRepository Teleporters { get; }
            public void Complete() { }
            public void Reject() { }
            public Microsoft.EntityFrameworkCore.Storage.IDbContextTransaction BeginTransaction() => throw new NotSupportedException();
            public void Dispose() { }
        }
    }
}
