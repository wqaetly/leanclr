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
    }
}
