namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunMemoryInstructions()
        {
            RunMemoryLdc();
            RunMemoryLdnull();
            RunMemoryLdloc();
            RunMemoryStloc();
        }

        public static void RunMemoryLdc()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Mems.TC_ldc));
        }

        public static void RunMemoryLdnull()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Mems.TC_ldnull));
        }

        public static void RunMemoryLdloc()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Mems.TC_ldloc));
        }

        public static void RunMemoryStloc()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Mems.TC_stloc));
        }
    }
}
