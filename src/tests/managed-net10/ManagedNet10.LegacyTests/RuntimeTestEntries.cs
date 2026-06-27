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
                typeof(Tests.CSharp.TC_Nullable));
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
    }
}
