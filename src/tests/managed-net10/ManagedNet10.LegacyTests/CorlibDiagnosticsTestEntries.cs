namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunCorlibDiagnostics()
        {
            RunCorlibDiagnosticsStopwatch();
        }

        public static void RunCorlibDiagnosticsStopwatch()
        {
            LegacyTestRunner.RunType(typeof(CorlibTests.InternalCall.TC_Stopwatch));
        }
    }
}
