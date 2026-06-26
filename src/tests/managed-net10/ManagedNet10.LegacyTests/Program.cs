namespace ManagedNet10.LegacyTests
{
    internal static class Program
    {
        private static void Main()
        {
            RunActivator();
        }

        public static void RunActivator()
        {
            ActivatorCreateInstanceClass();
            ActivatorCreateInstanceStruct();
            AOTActivatorCreateInstanceClass();
            AOTActivatorCreateInstanceStruct();
            TestStackAfterActivatorCreateInstanceClass();
            TestStackAfterActivatorCreateInstanceStruct();
            NewObjZeroArgument();
            CreateInt();
        }

        public static void ActivatorCreateInstanceClass()
        {
            new Tests.CSharp.TC_Activator().ActivatorCreateInstanceClass();
        }

        public static void ActivatorCreateInstanceStruct()
        {
            new Tests.CSharp.TC_Activator().ActivatorCreateInstanceStruct();
        }

        public static void AOTActivatorCreateInstanceClass()
        {
            new Tests.CSharp.TC_Activator().AOTActivatorCreateInstanceClass();
        }

        public static void AOTActivatorCreateInstanceStruct()
        {
            new Tests.CSharp.TC_Activator().AOTActivatorCreateInstanceStruct();
        }

        public static void TestStackAfterActivatorCreateInstanceClass()
        {
            new Tests.CSharp.TC_Activator().TestStackAfterActivatorCreateInstanceClass();
        }

        public static void TestStackAfterActivatorCreateInstanceStruct()
        {
            new Tests.CSharp.TC_Activator().TestStackAfterActivatorCreateInstanceStruct();
        }

        public static void NewObjZeroArgument()
        {
            new Tests.CSharp.TC_Activator().NewObjZeroArgument();
        }

        public static void CreateInt()
        {
            new Tests.CSharp.TC_Activator().CreateInt();
        }
    }
}
