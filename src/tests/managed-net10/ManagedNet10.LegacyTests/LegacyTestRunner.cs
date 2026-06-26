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

        public static int RunType(Type type)
        {
            if (Attribute.IsDefined(type, typeof(IgnoreTestAttribute), inherit: true))
            {
                return 0;
            }

            object instance = null;
            MethodInfo[] methods = type.GetMethods(TestMethodFlags);
            int executed = 0;

            for (int i = 0; i < methods.Length; i++)
            {
                MethodInfo method = methods[i];
                if (!Attribute.IsDefined(method, typeof(UnitTestAttribute), inherit: true))
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
    }
}
