namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunRuntimeBasics()
        {
            LegacyTestRunner.RunTypes(
                typeof(Tests.CSharp.TC_String),
                typeof(Tests.CSharp.TC_Enum),
                typeof(Tests.CSharp.TC_using));
        }

        public static void RunRuntimeLanguageFeatures()
        {
            LegacyTestRunner.RunTypes(
                typeof(Tests.CSharp.TC_foreach),
                typeof(Tests.CSharp.TC_event),
                typeof(Tests.CSharp.TC_Nullable),
                typeof(Tests.CSharp.TC_ArrayGenericInterface));
        }

        public static void RunRuntimeDelegates()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.Delegates.TC_Delegate_OpenClose));
        }

        public static void RunRuntimeDelegateReflectionProbe()
        {
            System.Span<byte> utf8Name = stackalloc byte[3];
            int written = System.Text.Encoding.UTF8.GetBytes("Run", utf8Name);
            Assert.Equal(3, written);
            Assert.Equal(82, (int)utf8Name[0]);
            Assert.Equal(117, (int)utf8Name[1]);
            Assert.Equal(110, (int)utf8Name[2]);
            System.Span<byte> expectedUtf8Name = stackalloc byte[3];
            expectedUtf8Name[0] = 82;
            expectedUtf8Name[1] = 117;
            expectedUtf8Name[2] = 110;
            Assert.IsTrue(System.MemoryExtensions.SequenceEqual<byte>(utf8Name, expectedUtf8Name));

            System.Type type = typeof(Tests.Fixtures.FT_AOT_Class);
            System.Reflection.BindingFlags flags =
                System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Static;

            System.Reflection.MethodInfo[] methods = type.GetMethods(flags);
            bool found = false;
            for (int i = 0; i < methods.Length; i++)
            {
                if (methods[i].Name == "Run")
                {
                    found = true;
                    break;
                }
            }
            Assert.IsTrue(found);

            System.Reflection.MethodInfo method = type.GetMethod("Run", flags);
            if (method == null)
            {
                Assert.Fail("FT_AOT_Class.GetMethod(Run) returned null; GetMethods count=" + methods.Length);
            }
            Assert.Equal("Run", method.Name);
        }

        public static void RunRuntimeString()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_String));
        }

        public static void RunRuntimeEnum()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_Enum));
        }

        public static void RunRuntimeUsing()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_using));
        }

        public static void RunRuntimeForeach()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_foreach));
        }

        public static void RunRuntimeEvent()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_event));
        }

        public static void RunRuntimeNullable()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_Nullable));
        }

        public static void RunRuntimeArrayGenericInterface()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_ArrayGenericInterface));
        }

        public static void RunRuntimeArrayGenericInterfaceClassGet()
        {
            LegacyTestRunner.RunMethod(typeof(Tests.CSharp.TC_ArrayGenericInterface), "class_get_1");
        }

        public static void RunRuntimeArrayGenericInterfaceClassSet()
        {
            LegacyTestRunner.RunMethod(typeof(Tests.CSharp.TC_ArrayGenericInterface), "class_set_1");
        }

        public static void RunRuntimeArrayGenericInterfaceStructGet()
        {
            LegacyTestRunner.RunMethod(typeof(Tests.CSharp.TC_ArrayGenericInterface), "struct_get_1");
        }

        public static void RunRuntimeArrayGenericInterfaceStructSet()
        {
            LegacyTestRunner.RunMethod(typeof(Tests.CSharp.TC_ArrayGenericInterface), "struct_set_1");
        }

        public static void RunRuntimeDelegateOpenClose()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.Delegates.TC_Delegate_OpenClose));
        }
    }
}
