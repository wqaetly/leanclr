namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunCorlibIO()
        {
            LegacyTestRunner.RunTypeWithNet10Replacements(typeof(CorlibTests.InternalCall.TC_System_IO_MonoIO));
        }
    }
}
