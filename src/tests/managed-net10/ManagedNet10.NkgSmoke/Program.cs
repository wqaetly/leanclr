using System.Diagnostics.CodeAnalysis;
using System.Reflection;

namespace ManagedNet10.NkgSmoke;

internal static class Program
{
    private static void Main()
    {
        RunCoreWorkloadSurfaceSmoke();
    }

    public static void RunCoreWorkloadSurfaceSmoke()
    {
        RunReflectionAttributeSmoke();
        RunAsyncAndSerializationSurfaceSmoke();
        RunSamplerCoreGameplaySmoke();
    }

    public static void RunReflectionAttributeSmoke()
    {
        var assembly = Assembly.Load("NKGGameFramework");
        Require(assembly.GetName().Name == "NKGGameFramework", "NKG assembly load failed");

        var references = assembly.GetReferencedAssemblies();
        Require(ContainsAssemblyReference(references, "OdinSerializer"), "NKG Odin reference missing");
        Require(ContainsAssemblyReference(references, "UniTask"), "NKG UniTask reference missing");

        var types = assembly.GetTypes();
        Require(types.Length > 0, "NKG type enumeration failed");

        bool sawCore = false;
        bool sawSerialization = false;
        int nkgTypeCount = 0;
        int reflectedMemberCount = 0;
        int customAttributeDataCount = 0;

        for (int i = 0; i < types.Length; i++)
        {
            var type = types[i];
            var ns = type.Namespace ?? "";
            if (!ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                continue;
            }

            nkgTypeCount++;
            sawCore |= ns == "NKGGameFramework.Core";
            sawSerialization |= ns == "NKGGameFramework.Serialization";
            customAttributeDataCount += type.GetCustomAttributesData().Count;

            const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

            var fields = type.GetFields(flags);
            reflectedMemberCount += fields.Length;
            for (int j = 0; j < fields.Length; j++)
            {
                customAttributeDataCount += fields[j].GetCustomAttributesData().Count;
            }

            var properties = type.GetProperties(flags);
            reflectedMemberCount += properties.Length;
            for (int j = 0; j < properties.Length; j++)
            {
                customAttributeDataCount += properties[j].GetCustomAttributesData().Count;
            }

            var methods = type.GetMethods(flags);
            reflectedMemberCount += methods.Length;
            for (int j = 0; j < methods.Length; j++)
            {
                customAttributeDataCount += methods[j].GetCustomAttributesData().Count;
            }
        }

        Require(sawCore, "NKG core namespace missing");
        Require(sawSerialization, "NKG serialization namespace missing");
        Require(nkgTypeCount >= 20, "NKG type count too small");
        Require(reflectedMemberCount >= 50, "NKG reflected member count too small");
        Require(customAttributeDataCount > 0, "NKG custom attribute data enumeration failed");

        var serializerType = assembly.GetType("NKGGameFramework.Serialization.OdinGameSerializer");
        Require(serializerType != null, "NKG OdinGameSerializer type lookup failed");
        Require(serializerType!.GetConstructors(BindingFlags.Public | BindingFlags.Instance).Length > 0, "NKG OdinGameSerializer constructors missing");
    }

    public static void RunAsyncAndSerializationSurfaceSmoke()
    {
        var assembly = Assembly.Load("NKGGameFramework");

        var gameAsyncType = RequireType(assembly, "NKGGameFramework.Async.GameAsync");
        var gameTimerType = RequireType(assembly, "NKGGameFramework.Core.IGameTimer");
        RequireProperty(gameAsyncType, "CompletedTask", "Cysharp.Threading.Tasks.UniTask");
        RequireMethod(gameAsyncType, "FromResult", null, parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(gameAsyncType, "WhenAll", "Cysharp.Threading.Tasks.UniTask", parameterCount: 1, genericArgumentCount: 0);
        RequireMethod(gameAsyncType, "WhenAll", null, parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(gameAsyncType, "WhenAny", null, parameterCount: 1, genericArgumentCount: 0);
        RequireMethod(gameAsyncType, "Delay", "Cysharp.Threading.Tasks.UniTask", parameterCount: 3, genericArgumentCount: 0);
        RequireMethod(gameAsyncType, "NextFrame", "Cysharp.Threading.Tasks.UniTask", parameterCount: 2, genericArgumentCount: 0);
        RequireMethod(gameAsyncType, "DelayFrame", "Cysharp.Threading.Tasks.UniTask", parameterCount: 3, genericArgumentCount: 0);
        RequireMethod(gameTimerType, "DelayAsync", "Cysharp.Threading.Tasks.UniTask", parameterCount: 2, genericArgumentCount: 0);
        RequireMethod(gameTimerType, "NextFrameAsync", "Cysharp.Threading.Tasks.UniTask", parameterCount: 1, genericArgumentCount: 0);
        RequireMethod(gameTimerType, "DelayFrameAsync", "Cysharp.Threading.Tasks.UniTask", parameterCount: 2, genericArgumentCount: 0);

        var gameSerializerType = RequireType(assembly, "NKGGameFramework.Serialization.IGameSerializer");
        var binarySerializerType = RequireType(assembly, "NKGGameFramework.Serialization.IBinaryGameSerializer");
        var jsonSerializerType = RequireType(assembly, "NKGGameFramework.Serialization.IJsonGameSerializer");
        var odinSerializerType = RequireType(assembly, "NKGGameFramework.Serialization.OdinGameSerializer");

        RequireMethod(gameSerializerType, "Serialize", "System.String", parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(gameSerializerType, "Deserialize", null, parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(binarySerializerType, "SerializeToBytes", "System.Byte[]", parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(binarySerializerType, "DeserializeFromBytes", null, parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(jsonSerializerType, "SerializeToJson", "System.String", parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(jsonSerializerType, "DeserializeFromJson", null, parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(odinSerializerType, "Serialize", "System.String", parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(odinSerializerType, "SerializeToBytes", "System.Byte[]", parameterCount: 1, genericArgumentCount: 1);
        RequireMethod(odinSerializerType, "SerializeToJson", "System.String", parameterCount: 1, genericArgumentCount: 1);
    }

    public static void RunSamplerCoreGameplaySmoke()
    {
        RunSampleGameUpdates(updateCount: 4, expectedFrame: 2, expectSnapshot: true);
    }

    public static void RunSamplerOdinRoundTripProbe()
    {
        RunSampleGameUpdates(updateCount: 5, expectedFrame: 3, expectSnapshot: false);
    }

    private static void RunSampleGameUpdates(int updateCount, int expectedFrame, bool expectSnapshot)
    {
        var assembly = Assembly.Load("NKGGameFramework.Sampler");
        var sampleGameType = RequireType(assembly, "NKGGameFramework.Sampler.SampleGame");
        var game = Activator.CreateInstance(sampleGameType, nonPublic: true);
        Require(game != null, "NKG SampleGame construction failed");

        InvokeInstance(sampleGameType, game!, "Start");
        Require((bool)RequirePropertyValue(sampleGameType, game!, "IsRunning"), "NKG SampleGame did not start");

        for (int i = 0; i < updateCount; i++)
        {
            InvokeInstance(sampleGameType, game!, "Update", 0.016d, 0.016d);
        }

        Require((int)RequirePropertyValue(sampleGameType, game!, "Frame") == expectedFrame, "NKG SampleGame frame progression failed");
        if (expectSnapshot)
        {
            var snapshot = InvokeInstance(sampleGameType, game!, "CreateSnapshot");
            Require(snapshot != null, "NKG SampleGame snapshot creation failed");
            var snapshotType = snapshot!.GetType();
            Require((int)RequirePropertyValue(snapshotType, snapshot, "Frame") == expectedFrame, "NKG SampleGame snapshot frame mismatch");
            Require((double)RequirePropertyValue(snapshotType, snapshot, "PositionX") > 0d, "NKG ECS movement did not update X position");
            Require((double)RequirePropertyValue(snapshotType, snapshot, "PositionY") > 0d, "NKG ECS movement did not update Y position");
            Require((int)RequirePropertyValue(snapshotType, snapshot, "Health") == 8, "NKG ECS damage system did not update health");
        }

        InvokeInstance(sampleGameType, game!, "Dispose");
    }

    public static void RunAssemblyNameSmoke()
    {
        var assembly = Assembly.Load("NKGGameFramework");
        Require(assembly.GetName().Name == "NKGGameFramework", "NKG assembly load failed");
    }

    public static void RunReferencedAssembliesSmoke()
    {
        var assembly = Assembly.Load("NKGGameFramework");
        var references = assembly.GetReferencedAssemblies();
        Require(ContainsAssemblyReference(references, "OdinSerializer"), "NKG Odin reference missing");
        Require(ContainsAssemblyReference(references, "UniTask"), "NKG UniTask reference missing");
    }

    public static void RunTypesSmoke()
    {
        var assembly = Assembly.Load("NKGGameFramework");
        var types = assembly.GetTypes();
        Require(types.Length > 0, "NKG type enumeration failed");
    }

    public static void RunTypeNamespaceSmoke()
    {
        var types = LoadNkgTypes();
        int nkgTypeCount = 0;
        bool sawCore = false;
        bool sawSerialization = false;

        for (int i = 0; i < types.Length; i++)
        {
            var ns = types[i].Namespace ?? "";
            if (!ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                continue;
            }

            nkgTypeCount++;
            sawCore |= ns == "NKGGameFramework.Core";
            sawSerialization |= ns == "NKGGameFramework.Serialization";
        }

        Require(sawCore, "NKG core namespace missing");
        Require(sawSerialization, "NKG serialization namespace missing");
        Require(nkgTypeCount >= 20, "NKG type count too small");
    }

    public static void RunTypeCustomAttributeDataSmoke()
    {
        var types = LoadNkgTypes();
        int customAttributeDataCount = 0;

        for (int i = 0; i < types.Length; i++)
        {
            var ns = types[i].Namespace ?? "";
            if (ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                var attrs = types[i].GetCustomAttributesData();
                customAttributeDataCount += attrs.Count;
            }
        }

        Require(customAttributeDataCount > 0, "NKG type custom attribute data enumeration failed");
    }

    public static void RunMemberEnumerationSmoke()
    {
        var types = LoadNkgTypes();
        int reflectedMemberCount = 0;

        for (int i = 0; i < types.Length; i++)
        {
            var ns = types[i].Namespace ?? "";
            if (!ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                continue;
            }

            const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;
            reflectedMemberCount += types[i].GetFields(flags).Length;
            reflectedMemberCount += types[i].GetProperties(flags).Length;
            reflectedMemberCount += types[i].GetMethods(flags).Length;
        }

        Require(reflectedMemberCount >= 50, "NKG reflected member count too small");
    }

    public static void RunMemberCustomAttributeDataSmoke()
    {
        int customAttributeDataCount =
            CountFieldCustomAttributeData() +
            CountPropertyCustomAttributeData() +
            CountMethodCustomAttributeData();

        Require(customAttributeDataCount > 0, "NKG member custom attribute data enumeration failed");
    }

    public static void RunFieldCustomAttributeDataSmoke()
    {
        _ = CountFieldCustomAttributeData();
    }

    public static void RunPropertyCustomAttributeDataSmoke()
    {
        _ = CountPropertyCustomAttributeData();
    }

    public static void RunMethodCustomAttributeDataSmoke()
    {
        _ = CountMethodCustomAttributeData();
    }

    private static int CountFieldCustomAttributeData()
    {
        var types = LoadNkgTypes();
        int customAttributeDataCount = 0;

        for (int i = 0; i < types.Length; i++)
        {
            var type = types[i];
            var ns = type.Namespace ?? "";
            if (!ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                continue;
            }

            const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

            var fields = type.GetFields(flags);
            for (int j = 0; j < fields.Length; j++)
            {
                customAttributeDataCount += fields[j].GetCustomAttributesData().Count;
            }
        }

        return customAttributeDataCount;
    }

    private static int CountPropertyCustomAttributeData()
    {
        var types = LoadNkgTypes();
        int customAttributeDataCount = 0;

        for (int i = 0; i < types.Length; i++)
        {
            var type = types[i];
            var ns = type.Namespace ?? "";
            if (!ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                continue;
            }

            const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

            var properties = type.GetProperties(flags);
            for (int j = 0; j < properties.Length; j++)
            {
                customAttributeDataCount += properties[j].GetCustomAttributesData().Count;
            }
        }

        return customAttributeDataCount;
    }

    private static int CountMethodCustomAttributeData()
    {
        var types = LoadNkgTypes();
        int customAttributeDataCount = 0;

        for (int i = 0; i < types.Length; i++)
        {
            var type = types[i];
            var ns = type.Namespace ?? "";
            if (!ns.StartsWith("NKGGameFramework", StringComparison.Ordinal))
            {
                continue;
            }

            const BindingFlags flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly;

            var methods = type.GetMethods(flags);
            for (int j = 0; j < methods.Length; j++)
            {
                customAttributeDataCount += methods[j].GetCustomAttributesData().Count;
            }
        }

        return customAttributeDataCount;
    }

    private static Type[] LoadNkgTypes()
    {
        return Assembly.Load("NKGGameFramework").GetTypes();
    }

    private static bool ContainsAssemblyReference(AssemblyName[] references, string name)
    {
        for (int i = 0; i < references.Length; i++)
        {
            if (references[i].Name == name)
            {
                return true;
            }
        }

        return false;
    }

    private static Type RequireType(Assembly assembly, string fullName)
    {
        var type = assembly.GetType(fullName);
        Require(type != null, fullName + " type lookup failed");
        return type!;
    }

    private static object? InvokeInstance(Type declaringType, object instance, string name, params object[] arguments)
    {
        var methods = declaringType.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        for (int i = 0; i < methods.Length; i++)
        {
            var method = methods[i];
            if (method.Name == name && method.GetParameters().Length == arguments.Length)
            {
                return method.Invoke(instance, arguments);
            }
        }

        throw new MissingMethodException(declaringType.FullName, name);
    }

    private static object RequirePropertyValue(Type declaringType, object instance, string name)
    {
        var property = declaringType.GetProperty(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        Require(property != null, declaringType.FullName + "." + name + " property missing");
        var value = property!.GetValue(instance);
        Require(value != null, declaringType.FullName + "." + name + " property returned null");
        return value!;
    }

    private static void RequireProperty(Type declaringType, string name, string expectedPropertyTypeName)
    {
        var property = declaringType.GetProperty(name, BindingFlags.Public | BindingFlags.Static);
        Require(property != null, declaringType.FullName + "." + name + " property missing");
        Require(TypeShapeName(property!.PropertyType) == expectedPropertyTypeName, declaringType.FullName + "." + name + " property type mismatch");
    }

    private static void RequireMethod(
        Type declaringType,
        string name,
        string? expectedReturnTypeName,
        int parameterCount,
        int genericArgumentCount)
    {
        var methods = declaringType.GetMethods(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly);
        for (int i = 0; i < methods.Length; i++)
        {
            var method = methods[i];
            if (method.Name != name ||
                method.GetParameters().Length != parameterCount ||
                method.GetGenericArguments().Length != genericArgumentCount)
            {
                continue;
            }

            if (expectedReturnTypeName == null || TypeShapeName(method.ReturnType) == expectedReturnTypeName)
            {
                return;
            }
        }

        throw new InvalidOperationException(declaringType.FullName + "." + name + " method surface missing");
    }

    private static string? TypeShapeName(Type type)
    {
        if (type.IsGenericType)
        {
            return type.GetGenericTypeDefinition().FullName;
        }

        return type.FullName;
    }

    private static void Require([DoesNotReturnIf(false)] bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
