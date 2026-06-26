namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        private static void Main()
        {
            RunAll();
        }

        public static void RunAll()
        {
            RunActivator();
            RunArithmeticInstructions();
            RunBranchInstructions();
            RunCompareInstructions();
        }

        public static void RunActivator()
        {
            ActivatorCreateInstanceClass();
            ActivatorCreateInstanceStruct();
            AOTActivatorCreateInstanceClass();
            AOTActivatorCreateInstanceStruct();
            TestStackAfterActivatorCreateInstanceClass();
            TestStackAfterActivatorCreateInstanceStruct();
            NewObjZeroArgument();
            CreateInt();
        }

        public static void ActivatorCreateInstanceClass()
        {
            new Tests.CSharp.TC_Activator().ActivatorCreateInstanceClass();
        }

        public static void ActivatorCreateInstanceStruct()
        {
            new Tests.CSharp.TC_Activator().ActivatorCreateInstanceStruct();
        }

        public static void AOTActivatorCreateInstanceClass()
        {
            new Tests.CSharp.TC_Activator().AOTActivatorCreateInstanceClass();
        }

        public static void AOTActivatorCreateInstanceStruct()
        {
            new Tests.CSharp.TC_Activator().AOTActivatorCreateInstanceStruct();
        }

        public static void TestStackAfterActivatorCreateInstanceClass()
        {
            new Tests.CSharp.TC_Activator().TestStackAfterActivatorCreateInstanceClass();
        }

        public static void TestStackAfterActivatorCreateInstanceStruct()
        {
            new Tests.CSharp.TC_Activator().TestStackAfterActivatorCreateInstanceStruct();
        }

        public static void NewObjZeroArgument()
        {
            new Tests.CSharp.TC_Activator().NewObjZeroArgument();
        }

        public static void CreateInt()
        {
            new Tests.CSharp.TC_Activator().CreateInt();
        }

        public static void RunArithmeticAdd()
        {
            RunArithmeticAddSmallIntegers();
            RunArithmeticAddIntegers();
            RunArithmeticAddNativeInteger();
            RunArithmeticAddFloatingPoint();
        }

        public static void RunArithmeticInstructions()
        {
            RunArithmeticAdd();
            RunArithmeticSub();
            RunArithmeticMul();
            RunArithmeticBitwise();
            RunArithmeticNeg();
        }

        public static void RunArithmeticAddSmallIntegers()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.byte_vv_1();
            test.byte_vc_1();
            test.byte_cv_1();
            test.sbyte_vv_1();
            test.sbyte_vc_1();
            test.sbyte_cv_1();
            test.short_vv_1();
            test.short_vc_1();
            test.short_cv_1();
            test.ushort_1();
        }

        public static void RunArithmeticAddIntegers()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.int_vv_1();
            test.int_vc_1();
            test.int_cv_1();
            test.int_vv_OverflowNotException();
            test.uint_vv_1();
            test.uint_vv_OverflowNotException();
            test.long_vv_1();
            test.long_vc_1();
            test.long_cv_1();
            test.long_vv_OverflowNotException();
            test.ulong_vv_1();
            test.ulong_vv_OverflowNotException();
        }

        public static void RunArithmeticAddNativeInteger()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.nint_int_vv_1();
        }

        public static void RunArithmeticAddFloatingPoint()
        {
            RunArithmeticAddFloatBasic();
            RunArithmeticAddFloatSpecial();
            RunArithmeticAddDoubleBasic();
            RunArithmeticAddDoubleSpecial();
        }

        public static void RunArithmeticAddFloatBasic()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.float_vv_1();
            test.float_vc_1();
            test.float_cv_1();
        }

        public static void RunArithmeticAddFloatSpecial()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.float_vv_inf();
            test.float_vv_minf();
            test.float_vv_NaN();
        }

        public static void RunArithmeticAddDoubleBasic()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.double_vv_1();
            test.double_vc_1();
            test.double_cv_1();
        }

        public static void RunArithmeticAddDoubleSpecial()
        {
            RunArithmeticAddDoubleInfinity();
            RunArithmeticAddDoubleNegativeInfinity();
            RunArithmeticAddDoubleNaN();
        }

        public static void RunArithmeticAddDoubleInfinity()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.double_inf();
        }

        public static void RunArithmeticAddDoubleNegativeInfinity()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.double_minf();
        }

        public static void RunArithmeticAddDoubleNaN()
        {
            var test = new Tests.Instruments.Ariths.TC_add();
            test.double_NaN();
        }

        public static void RunArithmeticSub()
        {
            Tests.Instruments.Ariths.TC_sub.byte_vv_1();
            Tests.Instruments.Ariths.TC_sub.sbyte_vv_1();
            Tests.Instruments.Ariths.TC_sub.short_vv_1();
            Tests.Instruments.Ariths.TC_sub.ushort_vv_1();
            Tests.Instruments.Ariths.TC_sub.int_vv_1();
            Tests.Instruments.Ariths.TC_sub.int_vc_1();
            Tests.Instruments.Ariths.TC_sub.int_cv_1();
            Tests.Instruments.Ariths.TC_sub.long_vv_1();
            Tests.Instruments.Ariths.TC_sub.long_vc_1();
            Tests.Instruments.Ariths.TC_sub.long_cv_1();
            Tests.Instruments.Ariths.TC_sub.nint_int_vv_1();
            Tests.Instruments.Ariths.TC_sub.Nuint_int_vv_1();
            Tests.Instruments.Ariths.TC_sub.float_vv_1();
            Tests.Instruments.Ariths.TC_sub.float_vv_inf();
            Tests.Instruments.Ariths.TC_sub.float_vv_minf();
            Tests.Instruments.Ariths.TC_sub.float_vv_NaN();
            Tests.Instruments.Ariths.TC_sub.double_vv_1();
            Tests.Instruments.Ariths.TC_sub.double_inf();
            Tests.Instruments.Ariths.TC_sub.double_minf();
            Tests.Instruments.Ariths.TC_sub.double_NaN();
        }

        public static void RunArithmeticMul()
        {
            Tests.Instruments.Ariths.TC_mul.byte_1();
            Tests.Instruments.Ariths.TC_mul.sbyte_1();
            Tests.Instruments.Ariths.TC_mul.short_1();
            Tests.Instruments.Ariths.TC_mul.ushort_1();
            Tests.Instruments.Ariths.TC_mul.int_1();
            Tests.Instruments.Ariths.TC_mul.uint_1();
            Tests.Instruments.Ariths.TC_mul.long_1();
            Tests.Instruments.Ariths.TC_mul.ulong_1();
            Tests.Instruments.Ariths.TC_mul.nint_1();
            Tests.Instruments.Ariths.TC_mul.float_1();
            Tests.Instruments.Ariths.TC_mul.float_NaN();
            Tests.Instruments.Ariths.TC_mul.float_NaN_1();
            Tests.Instruments.Ariths.TC_mul.double_1();
            Tests.Instruments.Ariths.TC_mul.double_NaN();
            Tests.Instruments.Ariths.TC_mul.double_NaN_1();
        }

        public static void RunArithmeticBitwise()
        {
            RunArithmeticAnd();
            RunArithmeticOr();
            RunArithmeticXor();
            RunArithmeticNot();
        }

        public static void RunArithmeticAnd()
        {
            var test = new Tests.Instruments.Ariths.TC_and();
            test.bool_true();
            test.bool_false();
            test.int_1();
            test.long_1();
        }

        public static void RunArithmeticOr()
        {
            var test = new Tests.Instruments.Ariths.TC_or();
            test.bool_1();
            test.bool_2();
            test.int_1();
            test.long_1();
        }

        public static void RunArithmeticXor()
        {
            var test = new Tests.Instruments.Ariths.TC_xor();
            test.int_1();
            test.long_1();
        }

        public static void RunArithmeticNot()
        {
            var test = new Tests.Instruments.Ariths.TC_not();
            test.int_1();
            test.uint_1();
            test.long_1();
            test.ulong_1();
            test.nint_1();
        }

        public static void RunArithmeticNeg()
        {
            var test = new Tests.Instruments.Ariths.TC_neg();
            test.sbyte_1();
            test.byte_1();
            test.short_1();
            test.ushort_1();
            test.int_1();
            test.long_1();
            test.float_1();
            test.float_inf();
            test.float_NaN();
            test.double_1();
            test.double_inf();
            test.double_NaN();
        }
    }
}
