using System;
using System.Reflection;

namespace ManagedNet10.LegacyTests
{
    internal static class LegacyTestRunner
    {
        private const BindingFlags TestMethodFlags =
            BindingFlags.Public |
            BindingFlags.NonPublic |
            BindingFlags.Instance |
            BindingFlags.Static;

        public static int RunTypes(params Type[] types)
        {
            int executed = 0;
            for (int i = 0; i < types.Length; i++)
            {
                executed += RunType(types[i]);
            }

            return executed;
        }

        public static int RunAssembly(Assembly assembly)
        {
            int executed = 0;
            Type[] types;
            try
            {
                types = assembly.GetTypes();
            }
            catch (Exception ex)
            {
                throw new Exception("Failed to enumerate legacy test types in " + assembly.FullName, ex);
            }

            for (int i = 0; i < types.Length; i++)
            {
                executed += RunType(types[i], skipNet10ReplacedLegacyTests: true);
            }

            if (executed == 0)
            {
                throw new Exception("No UnitTest methods executed for " + assembly.FullName);
            }

            return executed;
        }

        public static int RunType(Type type)
        {
            return RunType(type, skipNet10ReplacedLegacyTests: false);
        }

        public static int RunTypeWithNet10Replacements(Type type)
        {
            return RunType(type, skipNet10ReplacedLegacyTests: true);
        }

        private static int RunType(Type type, bool skipNet10ReplacedLegacyTests)
        {
            bool isIgnored;
            try
            {
                isIgnored = Attribute.IsDefined(type, typeof(IgnoreTestAttribute), inherit: true);
            }
            catch (Exception ex)
            {
                throw new Exception("Failed to inspect legacy test attributes on type " + type.FullName, ex);
            }

            if (isIgnored)
            {
                return 0;
            }

            object instance = null;
            MethodInfo[] methods;
            try
            {
                methods = type.GetMethods(TestMethodFlags);
            }
            catch (Exception ex)
            {
                throw new Exception("Failed to enumerate legacy test methods on " + type.FullName, ex);
            }

            int executed = 0;

            for (int i = 0; i < methods.Length; i++)
            {
                MethodInfo method = methods[i];
                bool isUnitTest;
                try
                {
                    isUnitTest = Attribute.IsDefined(method, typeof(UnitTestAttribute), inherit: true);
                }
                catch (Exception ex)
                {
                    throw new Exception("Failed to inspect legacy test attributes on " + type.FullName + "." + method.Name, ex);
                }

                if (!isUnitTest)
                {
                    continue;
                }
                if (skipNet10ReplacedLegacyTests && IsNet10ReplacedLegacyTest(type, method.Name))
                {
                    continue;
                }
                if (method.ReturnType != typeof(void) || method.GetParameters().Length != 0)
                {
                    continue;
                }

                if (!method.IsStatic && instance == null)
                {
                    instance = Activator.CreateInstance(type);
                }

                method.Invoke(instance, null);
                executed++;
            }

            return executed;
        }

        public static void RunMethod(Type type, string methodName)
        {
            MethodInfo method = type.GetMethod(methodName, TestMethodFlags);
            if (method == null)
            {
                throw new MissingMethodException(type.FullName, methodName);
            }
            if (method.ReturnType != typeof(void) || method.GetParameters().Length != 0)
            {
                throw new InvalidOperationException(type.FullName + "." + methodName + " is not a zero-argument void test method.");
            }

            object instance = method.IsStatic ? null : Activator.CreateInstance(type);
            method.Invoke(instance, null);
        }

        private static bool IsNet10ReplacedLegacyTest(Type type, string methodName)
        {
            // The linked legacy source still carries Mono/mscorlib expectations for this case.
            // RunAssembly uses the net10-specific replacement in CorlibStringNet10Semantics instead.
            if (type == typeof(CorlibTests.InternalCall.TC_System_String) &&
                methodName == "LastIndexOf_EmptyString")
            {
                return true;
            }

            // The original assembly name test was compiled into CorlibTests.dll. The linked
            // net10 assembly intentionally runs under ManagedNet10.LegacyTests instead.
            if (type == typeof(CorlibTests.InternalCall.TC_System_Reflection_AssemblyName) &&
                methodName == "GetNativeName")
            {
                return true;
            }

            if (type == typeof(CorlibTests.InternalCall.TC_System_Reflection_RuntimeAssembly))
            {
                return methodName == "GetFullName" ||
                    methodName == "GetEntryPoint" ||
                    methodName == "GetManifestModule";
            }

            if (type == typeof(CorlibTests.InternalCall.TC_System_Reflection_RuntimeFieldInfo))
            {
                return methodName == "GetRawConstantValue_ForConstField";
            }

            return false;
        }
    }
}
