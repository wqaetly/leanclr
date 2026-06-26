namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunConvertInstructions()
        {
            RunConvertSignedInteger();
            RunConvertFloatingPoint();
        }

        public static void RunConvertSignedInteger()
        {
            RunConvertI1();
            RunConvertI2();
            RunConvertI4();
            RunConvertI8();
        }

        public static void RunConvertI1()
        {
            var test = new Tests.Instruments.Converts.TC_conv_i1();
            test.byte_1();
            test.byte_2();
            test.short_1();
            test.short_overflow_up();
            test.short_overflow_down();
            test.ushort_1();
            test.ushort_overflow_up();
            test.char_1();
            test.char_overflow_up();
            test.int_1();
            test.int_overflow_up();
            test.int_overflow_down();
            test.uint_1();
            test.uint_overflow_up();
            test.long_1();
            test.long_overflow_up();
            test.long_overflow_down();
            test.ulong_1();
            test.ulong_overflow_up();
            test.nint_1();
            test.nint_overflow_up();
            test.nint_overflow_down();
            test.unint_1();
            test.unint_overflow_up();
            test.float_1();
            test.float_overflow_up();
            test.float_overflow_down();
            test.double_1();
            test.double_overflow_up();
            test.double_overflow_down();
            test.ConvI4ToI1();
        }

        public static void RunConvertI2()
        {
            var test = new Tests.Instruments.Converts.TC_conv_i2();
            test.byte_1();
            test.sbyte_1();
            test.sbyte_2();
            test.ushort_1();
            test.ushort_overflow_up();
            test.char_1();
            test.char_overflow_up();
            test.int_1();
            test.int_overflow_up();
            test.int_overflow_down();
            test.uint_1();
            test.uint_overflow_up();
            test.long_1();
            test.long_overflow_up();
            test.long_overflow_down();
            test.ulong_1();
            test.ulong_overflow_up();
            test.nint_1();
            test.nint_overflow_up();
            test.nint_overflow_down();
            test.unint_1();
            test.unint_overflow_up();
            test.float_1();
            test.float_overflow_up();
            test.float_overflow_down();
            test.double_1();
            test.double_overflow_up();
            test.double_overflow_down();
            test.ConvI4ToI2();
        }

        public static void RunConvertI4()
        {
            var test = new Tests.Instruments.Converts.TC_conv_i4();
            test.byte_1();
            test.sbyte_1();
            test.sbyte_2();
            test.short_1();
            test.short_2();
            test.ushort_1();
            test.char_1();
            test.uint_1();
            test.uint_overflow_up();
            test.long_1();
            test.long_overflow_up();
            test.long_overflow_down();
            test.ulong_1();
            test.ulong_overflow_up();
            test.nint_1();
            test.nint_overflow_up();
            test.nint_overflow_down();
            test.unint_1();
            test.unint_overflow_up();
            test.float_1();
            test.double_1();
        }

        public static void RunConvertI8()
        {
            var test = new Tests.Instruments.Converts.TC_conv_i8();
            test.byte_1();
            test.sbyte_1();
            test.sbyte_2();
            test.short_1();
            test.short_2();
            test.ushort_1();
            test.char_1();
            test.int_1();
            test.int_2();
            test.uint_1();
            test.uint_2();
            test.ulong_1();
            test.ulong_overflow_up();
            test.nint_1();
            test.float_1();
            test.float_2();
            test.double_1();
            test.double_2();
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
