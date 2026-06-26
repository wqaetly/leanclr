namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunConvertInstructions()
        {
            RunConvertFloatingPoint();
        }

        public static void RunConvertFloatingPoint()
        {
            RunConvertR4();
            RunConvertR8();
            RunConvertRUn();
        }

        public static void RunConvertR4()
        {
            var test = new Tests.Instruments.Converts.TC_conv_r4();
            test.byte_1();
            test.sbyte_1();
            test.sbyte_2();
            test.short_1();
            test.short_2();
            test.ushort_1();
            test.char_1();
            test.int_1();
            test.int_2();
            test.long_1();
            test.nint_1();
            test.float_1();
            test.double_1();
        }

        public static void RunConvertR8()
        {
            var test = new Tests.Instruments.Converts.TC_conv_r8();
            test.byte_1();
            test.sbyte_1();
            test.sbyte_2();
            test.short_1();
            test.short_2();
            test.ushort_1();
            test.char_1();
            test.int_1();
            test.int_2();
            test.long_1();
            test.nint_1();
            test.float_1();
            test.double_1();
        }

        public static void RunConvertRUn()
        {
            var test = new Tests.Instruments.Converts.TC_conv_r_un();
            test.uint_1();
            test.ulong_1();
        }
    }
}
