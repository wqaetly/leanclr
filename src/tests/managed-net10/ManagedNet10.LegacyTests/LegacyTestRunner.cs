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

        public static void RunTypes(params Type[] types)
        {
            for (int i = 0; i < types.Length; i++)
            {
                RunType(types[i]);
            }
        }

        public static void RunType(Type type)
        {
            object instance = null;
            MethodInfo[] methods = type.GetMethods(TestMethodFlags);

            for (int i = 0; i < methods.Length; i++)
            {
                MethodInfo method = methods[i];
                if (!Attribute.IsDefined(method, typeof(UnitTestAttribute), inherit: true))
                {
                    continue;
                }

                if (instance == null)
                {
                    instance = Activator.CreateInstance(type);
                }

                method.Invoke(instance, null);
            }
        }
    }
}
