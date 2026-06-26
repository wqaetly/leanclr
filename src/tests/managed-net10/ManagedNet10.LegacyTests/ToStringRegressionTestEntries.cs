namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunToStringRegressions()
        {
            LegacyTestRunner.RunTypes(
                typeof(Tests.Bugs.Bug_ValueTypeToString_2022_7_22),
                typeof(Tests.Bugs.Bug20220927.Bug_2022_9_27));
        }
    }
}
