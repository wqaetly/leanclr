namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunMemoryInstructions()
        {
            RunMemoryLdc();
            RunMemoryLdnull();
            RunMemoryLdloc();
            RunMemoryStloc();
        }

        public static void RunMemoryLdc()
        {
            var test = new Tests.Instruments.Mems.TC_ldc();
            test.ldc_i4_0();
            test.ldc_i4_1();
            test.ldc_i4_2();
            test.ldc_i4_3();
            test.ldc_i4_4();
            test.ldc_i4_5();
            test.ldc_i4_6();
            test.ldc_i4_7();
            test.ldc_i4_8();
            test.ldc_i4_m1();
            test.ldc_i4_9();
            test.ldc_i8_0xFFFFFFFFF();
            test.ldc_r4_999();
            test.ldc_r8_999();
        }

        public static void RunMemoryLdnull()
        {
            Tests.Instruments.Mems.TC_ldnull.ldnull();
        }

        public static void RunMemoryLdloc()
        {
            var test = new Tests.Instruments.Mems.TC_ldloc();
            test.byte_1();
            test.sbyte_1();
            test.sbyte_2();
            test.ushort_1();
            test.short_1();
            test.short_2();
            test.uint_1();
            test.int_1();
            test.int_2();
            test.ulong_1();
            test.long_1();
            test.long_2();
            test.unint_1();
            test.nint_1();
            test.nint_2();
            test.float_1();
            test.double_1();
            test.p_0();
            test.p_1();
            test.p_2();
            test.p_3();
            test.p_s();
            test.s_253();
            test.p_255();
        }

        public static void RunMemoryStloc()
        {
            Tests.Instruments.Mems.TC_stloc.stloc_0();
            Tests.Instruments.Mems.TC_stloc.stloc_1();
            Tests.Instruments.Mems.TC_stloc.stloc_2();
            Tests.Instruments.Mems.TC_stloc.stloc_3();
            Tests.Instruments.Mems.TC_stloc.stloc_s_4();
            Tests.Instruments.Mems.TC_stloc.stloc_s_255();
            Tests.Instruments.Mems.TC_stloc.stloc_6001();
        }
    }
}
