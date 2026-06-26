namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunCompareInstructions()
        {
            RunCompareCeq();
            RunCompareCgt();
            RunCompareCgtUn();
            RunCompareClt();
            RunCompareCltUn();
        }

        public static void RunCompareCeq()
        {
            var test = new Tests.Instruments.Cmps.TC_Ceq();
            test.int_1();
            test.int_2();
            test.long_1();
            test.long_2();
            test.nint_1();
            test.nint_2();
            test.float_1();
            test.float_2();
            test.double_1();
            test.double_2();
            test.ref_1();
            test.ref_2();
            test.object_true();
            test.object_false();
        }

        public static void RunCompareCgt()
        {
            var test = new Tests.Instruments.Cmps.TC_Cgt();
            test.int_true();
            test.int_false();
            test.long_true();
            test.long_false();
            test.nint_true();
            test.nint_false();
            test.float_true();
            test.float_false();
            test.float_NaN();
            test.float_NInf();
            test.float_PInf();
            test.double_true();
            test.double_false();
            test.double_NaN();
            test.double_NInf();
            test.double_PInf();
        }

        public static void RunCompareCgtUn()
        {
            var test = new Tests.Instruments.Cmps.TC_Cgt_un();
            test.uint_false();
            test.uint_true();
            test.ulong_false();
            test.ulong_true();
            test.float_false();
            test.float_true();
            test.double_false();
            test.double_true();
        }

        public static void RunCompareClt()
        {
            var test = new Tests.Instruments.Cmps.TC_Clt();
            test.int_true();
            test.int_false();
            test.long_true();
            test.long_false();
            test.nint_true();
            test.nint_false();
            test.float_true();
            test.float_false();
            test.float_false_1();
            test.float_false_2();
            test.float_false_3();
            test.float_false_4();
            test.float_false_5();
            test.float_double_false();
            test.double_true();
            test.double_false();
            test.double_false_1();
            test.double_false_2();
            test.double_false_3();
            test.double_false_4();
            test.double_false_5();
        }

        public static void RunCompareCltUn()
        {
            var test = new Tests.Instruments.Cmps.TC_Clt_un();
            test.uint_false();
            test.uint_true();
            test.ulong_true();
            test.ulong_false();
            test.unint_false();
            test.unint_true();
        }
    }
}
