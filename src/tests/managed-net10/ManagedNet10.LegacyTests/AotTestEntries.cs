namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunAotCallInterp()
        {
            LegacyTestRunner.RunType(typeof(TestCall));
        }

        public static void RunAotCallVirInterp()
        {
            LegacyTestRunner.RunType(typeof(TestCallVir));
        }

        public static void RunAotCallInterop()
        {
            RunAotCallInterp();
            RunAotCallVirInterp();
        }
    }
}
