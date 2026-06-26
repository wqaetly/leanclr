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
            RunConvertInstructions();
            RunMemoryInstructions();
        }

        public static void RunActivator()
        {
            LegacyTestRunner.RunType(typeof(Tests.CSharp.TC_Activator));
        }

        public static void ActivatorCreateInstanceClass()
        {
            RunActivator();
        }

        public static void ActivatorCreateInstanceStruct()
        {
            RunActivator();
        }

        public static void AOTActivatorCreateInstanceClass()
        {
            RunActivator();
        }

        public static void AOTActivatorCreateInstanceStruct()
        {
            RunActivator();
        }

        public static void TestStackAfterActivatorCreateInstanceClass()
        {
            RunActivator();
        }

        public static void TestStackAfterActivatorCreateInstanceStruct()
        {
            RunActivator();
        }

        public static void NewObjZeroArgument()
        {
            RunActivator();
        }

        public static void CreateInt()
        {
            RunActivator();
        }

        public static void RunArithmeticInstructions()
        {
            RunArithmeticAdd();
            RunArithmeticSub();
            RunArithmeticMul();
            RunArithmeticDivRem();
            RunArithmeticBitwise();
            RunArithmeticNeg();
            RunArithmeticShift();
        }

        public static void RunArithmeticAdd()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_add));
        }

        public static void RunArithmeticAddSmallIntegers()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddIntegers()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddNativeInteger()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddFloatingPoint()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddFloatBasic()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddFloatSpecial()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddDoubleBasic()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddDoubleSpecial()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddDoubleInfinity()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddDoubleNegativeInfinity()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticAddDoubleNaN()
        {
            RunArithmeticAdd();
        }

        public static void RunArithmeticSub()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_sub));
        }

        public static void RunArithmeticMul()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_mul));
        }

        public static void RunArithmeticDivRem()
        {
            RunArithmeticDiv();
            RunArithmeticDivUn();
            RunArithmeticRem();
            RunArithmeticRemUn();
        }

        public static void RunArithmeticDiv()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_div));
        }

        public static void RunArithmeticDivUn()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_div_un));
        }

        public static void RunArithmeticRem()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_rem));
        }

        public static void RunArithmeticRemUn()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_un));
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
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_and));
        }

        public static void RunArithmeticOr()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_or));
        }

        public static void RunArithmeticXor()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_xor));
        }

        public static void RunArithmeticNot()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_not));
        }

        public static void RunArithmeticNeg()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_neg));
        }

        public static void RunArithmeticShift()
        {
            RunArithmeticShl();
            RunArithmeticShr();
            RunArithmeticShrUn();
        }

        public static void RunArithmeticShl()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_shl));
        }

        public static void RunArithmeticShr()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_shr));
        }

        public static void RunArithmeticShrUn()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Ariths.TC_shr_un));
        }
    }
}
