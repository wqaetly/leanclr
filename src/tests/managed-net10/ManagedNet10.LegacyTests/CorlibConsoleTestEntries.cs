namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunCorlibConsole()
        {
            LegacyTestRunner.RunTypeWithNet10Replacements(typeof(CorlibTests.InternalCall.TC_System_Console));
        }
    }
}
