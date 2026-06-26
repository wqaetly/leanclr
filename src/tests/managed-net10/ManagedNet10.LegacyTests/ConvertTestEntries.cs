namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunConvertInstructions()
        {
            RunConvertSignedInteger();
            RunConvertUnsignedInteger();
            RunConvertFloatingPoint();
        }

        public static void RunConvertUnsignedInteger()
        {
            RunConvertU1();
            RunConvertU2();
            RunConvertU4();
            RunConvertU8();
        }

        public static void RunConvertU1()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_u1));
        }

        public static void RunConvertU2()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_u2));
        }

        public static void RunConvertU4()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_u4));
        }

        public static void RunConvertU8()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_u8));
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
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_i1));
        }

        public static void RunConvertI2()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_i2));
        }

        public static void RunConvertI4()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_i4));
        }

        public static void RunConvertI8()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_i8));
        }

        public static void RunConvertFloatingPoint()
        {
            RunConvertR4();
            RunConvertR8();
            RunConvertRUn();
        }

        public static void RunConvertR4()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_r4));
        }

        public static void RunConvertR8()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_r8));
        }

        public static void RunConvertRUn()
        {
            LegacyTestRunner.RunType(typeof(Tests.Instruments.Converts.TC_conv_r_un));
        }
    }
}
