using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using Rasa.Missions.Content;

namespace Harness
{
    // The contract System.Text.Json reads Rasa.NET's scene documents with (MissionContentCodec.Options): every type reachable from
    // MissionSceneDefinition and MissionExperienceDefinition, as JsonTypeInfo describes it - objects with their JSON property
    // names, types, required flags and polymorphism, lists, dictionaries, and the leaf types. RasaEdit's MissionSceneSchema.h is made
    // from this file (tools/missioncheck/make_scene_schema.py), so the editor checks documents against the server's own contract.
    internal static class SceneSchema
    {
        public static int Write(string path)
        {
            var options = MissionContentCodec.Options;
            if (!options.IsReadOnly) options.MakeReadOnly(populateMissingResolver: true);
            var types = new SortedDictionary<string, object>(StringComparer.Ordinal);
            var pending = new Queue<Type>(new[] { typeof(MissionSceneDefinition), typeof(MissionExperienceDefinition) });
            var seen = new HashSet<Type>();
            string Name(Type t) => Describe(t, pending, seen, options);
            while (pending.Count > 0)
            {
                var type = pending.Dequeue();
                var info = options.GetTypeInfo(type);
                var entry = new Dictionary<string, object>();
                if (info.Kind == JsonTypeInfoKind.Object)
                {
                    entry["kind"] = "object";
                    entry["abstract"] = type.IsAbstract;
                    entry["properties"] = info.Properties.Select(p => new Dictionary<string, object> {
                        ["name"] = p.Name, ["type"] = Name(p.PropertyType), ["required"] = p.IsRequired,
                        ["settable"] = p.Set != null || p.AssociatedParameter != null }).ToList();
                    var poly = info.PolymorphismOptions;
                    if (poly != null)
                    {
                        entry["discriminator"] = poly.TypeDiscriminatorPropertyName;
                        entry["unknownDerived"] = poly.UnknownDerivedTypeHandling.ToString();
                        entry["derived"] = poly.DerivedTypes.Select(d => new Dictionary<string, object> {
                            ["id"] = d.TypeDiscriminator?.ToString(), ["type"] = Name(d.DerivedType) }).ToList();
                    }
                    if (info.Converter.GetType().Namespace?.StartsWith("System.Text.Json") != true)
                        entry["converter"] = info.Converter.GetType().FullName;
                }
                types[TypeKey(type)] = entry;
            }
            File.WriteAllText(path, JsonSerializer.Serialize(new Dictionary<string, object> {
                ["roots"] = new[] { TypeKey(typeof(MissionSceneDefinition)), TypeKey(typeof(MissionExperienceDefinition)) },
                ["types"] = types }, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"scene schema: {types.Count} object types -> {path}");
            return 0;
        }

        // Each line of `path` is one document: deserialised as MissionSceneDefinition ("scene") or MissionExperienceDefinition
        // ("experience") with MissionContentCodec.Options, as MissionContentCatalog.Load does; prints "ok" or the exception per line.
        public static int Read(string root, string path)
        {
            var type = root == "experience" ? typeof(MissionExperienceDefinition) : typeof(MissionSceneDefinition);
            foreach (var line in File.ReadLines(path))
            {
                try
                {
                    JsonSerializer.Deserialize(line, type, MissionContentCodec.Options);
                    Console.WriteLine("ok");
                }
                catch (Exception e)
                {
                    Console.WriteLine("FAIL " + e.GetType().Name + ": " + e.Message.Replace('\n', ' ').Replace('\r', ' '));
                }
            }
            return 0;
        }

        private static string TypeKey(Type t) => t.FullName?.Replace('+', '.') ?? t.Name;

        // A type expression: "string", "bool", "uint", "int", "float", "double", "ulong", "long", "ushort", "short", "byte",
        // "sbyte", "json" (JsonElement / object: anything), "enum:<Name>:<flags 0/1>:<name=value,...>", "nullable:<inner>",
        // "list:<element>", "dict:<key>:<value>", "obj:<type key>" (queued for its own entry), "converter:<name>" (custom).
        private static string Describe(Type t, Queue<Type> pending, HashSet<Type> seen, JsonSerializerOptions options)
        {
            var under = Nullable.GetUnderlyingType(t);
            if (under != null) return "nullable:" + Describe(under, pending, seen, options);
            if (t == typeof(string)) return "string";
            if (t == typeof(bool)) return "bool";
            if (t == typeof(uint)) return "uint";
            if (t == typeof(int)) return "int";
            if (t == typeof(float)) return "float";
            if (t == typeof(double)) return "double";
            if (t == typeof(decimal)) return "double";
            if (t == typeof(ulong)) return "ulong";
            if (t == typeof(long)) return "long";
            if (t == typeof(ushort)) return "ushort";
            if (t == typeof(short)) return "short";
            if (t == typeof(byte)) return "byte";
            if (t == typeof(sbyte)) return "sbyte";
            if (t == typeof(JsonElement) || t == typeof(object)) return "json";
            if (t.IsEnum)
            {
                var flags = t.IsDefined(typeof(FlagsAttribute), false);
                var names = Enum.GetNames(t).Select(n => n + "=" + Convert.ToInt64(Enum.Parse(t, n)));
                return "enum:" + t.Name + ":" + (flags ? 1 : 0) + ":" + string.Join(",", names);
            }
            var info = options.GetTypeInfo(t);
            var converter = info.Converter.GetType();
            if (converter.Namespace?.StartsWith("System.Text.Json") != true) return "converter:" + converter.FullName;
            switch (info.Kind)
            {
                case JsonTypeInfoKind.Enumerable:
                    return "list:" + Describe(info.ElementType, pending, seen, options);
                case JsonTypeInfoKind.Dictionary:
                    return "dict:" + Describe(info.KeyType, pending, seen, options) + ":" + Describe(info.ElementType, pending, seen, options);
                case JsonTypeInfoKind.Object:
                    if (seen.Add(t)) pending.Enqueue(t);
                    return "obj:" + TypeKey(t);
                default:
                    return "unsupported:" + t.FullName;
            }
        }
    }
}
