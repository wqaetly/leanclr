namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunCorlibThreading()
        {
            RunCorlibInterlocked();
            RunCorlibVolatile();
            RunCorlibThread();
        }

        public static void RunCorlibThread()
        {
            LegacyTestRunner.RunTypeWithNet10Replacements(typeof(CorlibTests.InternalCall.TC_System_Threading_Thread));
        }

        public static void RunCorlibInterlocked()
        {
            LegacyTestRunner.RunType(typeof(CorlibTests.InternalCall.TC_System_Threading_Interlocked));
        }

        public static void RunCorlibVolatile()
        {
            LegacyTestRunner.RunType(typeof(CorlibTests.InternalCall.TC_System_Threading_Volatile));
        }
    }
}
