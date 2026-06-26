namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunFunctionLdftnDelegates()
        {
            LegacyTestRunner.RunTypes(
                typeof(Tests.Instruments.Funcs.TC_ldftn),
                typeof(Tests.Instruments.Funcs.TC_ldvirftn));
        }

        public static void RunFunctionCallBasic()
        {
            LegacyTestRunner.RunTypes(
                typeof(Tests.Instruments.Funcs.TC_call_aot),
                typeof(Tests.Instruments.Funcs.TC_call_interp));
        }

        public static void RunFunctionCallAot()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Funcs.TC_call_aot));
        }

        public static void RunFunctionCallInterp()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Funcs.TC_call_interp));
        }

        public static void RunFunctionLdftn()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Funcs.TC_ldftn));
        }

        public static void RunFunctionLdvirtftn()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Funcs.TC_ldvirftn));
        }
    }
}
