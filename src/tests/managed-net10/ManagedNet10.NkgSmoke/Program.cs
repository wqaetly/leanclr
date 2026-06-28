using System.Diagnostics.CodeAnalysis;
using System.Reflection;

namespace ManagedNet10.NkgSmoke;

internal static class Program
{
    private static void Main()
    {
        RunReflectionAttributeSmoke();
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

    private static void Require([DoesNotReturnIf(false)] bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
