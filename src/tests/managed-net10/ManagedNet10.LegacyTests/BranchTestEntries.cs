namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunBranchInstructions()
        {
            RunBranchBr();
            RunBranchBrtrue();
            RunBranchBrfalse();
            RunBranchBeq();
            RunBranchBneUn();
            RunBranchSwitch();
        }

        public static void RunBranchBr()
        {
            Tests.Instruments.Branches.TC_Br.s_blank();
            Tests.Instruments.Branches.TC_Br.s_forward();
            Tests.Instruments.Branches.TC_Br.s_back();
            Tests.Instruments.Branches.TC_Br.far_forward();
            Tests.Instruments.Branches.TC_Br.far_back();
        }

        public static void RunBranchBrtrue()
        {
            Tests.Instruments.Branches.TC_brtrue.s_blank();
            Tests.Instruments.Branches.TC_brtrue.s_true();
            Tests.Instruments.Branches.TC_brtrue.true_long();
            Tests.Instruments.Branches.TC_brtrue.s_false();
            Tests.Instruments.Branches.TC_brtrue.far_1();
            Tests.Instruments.Branches.TC_brtrue.far_2();
        }

        public static void RunBranchBrfalse()
        {
            Tests.Instruments.Branches.TC_Brfalse.s_blank();
            Tests.Instruments.Branches.TC_Brfalse.s_true();
            Tests.Instruments.Branches.TC_Brfalse.s_false();
            Tests.Instruments.Branches.TC_Brfalse.far_false();
            Tests.Instruments.Branches.TC_Brfalse.far_true();
        }

        public static void RunBranchSwitch()
        {
            Tests.Instruments.Branches.TC_switch.all_empty_case();
            Tests.Instruments.Branches.TC_switch.seq_1();
            Tests.Instruments.Branches.TC_switch.seq_2();
            Tests.Instruments.Branches.TC_switch.seq_3();
            Tests.Instruments.Branches.TC_switch.not_seq_1();
            Tests.Instruments.Branches.TC_switch.not_seq_2();
            Tests.Instruments.Branches.TC_switch.not_seq_3();
            Tests.Instruments.Branches.TC_switch.s_switch();
        }

        public static void RunBranchBeq()
        {
            Tests.Instruments.Branches.TC_Beq.s_blank();
            Tests.Instruments.Branches.TC_Beq.s_int_1();
            Tests.Instruments.Branches.TC_Beq.s_int_2();
            Tests.Instruments.Branches.TC_Beq.s_long_1();
            Tests.Instruments.Branches.TC_Beq.s_long_2();
            Tests.Instruments.Branches.TC_Beq.s_nint_1();
            Tests.Instruments.Branches.TC_Beq.s_nint_2();
            Tests.Instruments.Branches.TC_Beq.s_float_1();
            Tests.Instruments.Branches.TC_Beq.s_float_2();
            Tests.Instruments.Branches.TC_Beq.s_double_1();
            Tests.Instruments.Branches.TC_Beq.s_double_2();
            Tests.Instruments.Branches.TC_Beq.s_object_1();
            Tests.Instruments.Branches.TC_Beq.s_object_2();
            Tests.Instruments.Branches.TC_Beq.int_1();
            Tests.Instruments.Branches.TC_Beq.int_2();
            Tests.Instruments.Branches.TC_Beq.long_1();
            Tests.Instruments.Branches.TC_Beq.long_2();
            Tests.Instruments.Branches.TC_Beq.object_1();
            Tests.Instruments.Branches.TC_Beq.object_2();
            Tests.Instruments.Branches.TC_Beq.nint_1();
            Tests.Instruments.Branches.TC_Beq.nint_2();

            var test = new Tests.Instruments.Branches.TC_Beq();
            test.CmpInfinite_float();
            test.CmpInfinite_double();
        }

        public static void RunBranchBneUn()
        {
            Tests.Instruments.Branches.TC_Bne_un.s_blank();
            Tests.Instruments.Branches.TC_Bne_un.s_uint_1();
            Tests.Instruments.Branches.TC_Bne_un.s_uint_2();
            Tests.Instruments.Branches.TC_Bne_un.s_ulong_1();
            Tests.Instruments.Branches.TC_Bne_un.s_ulong_2();
            Tests.Instruments.Branches.TC_Bne_un.s_nint_1();
            Tests.Instruments.Branches.TC_Bne_un.s_nint_2();
            Tests.Instruments.Branches.TC_Bne_un.s_object_1();
            Tests.Instruments.Branches.TC_Bne_un.s_object_2();
            Tests.Instruments.Branches.TC_Bne_un.s_float_1();
            Tests.Instruments.Branches.TC_Bne_un.s_float_2();
            Tests.Instruments.Branches.TC_Bne_un.s_double_1();
            Tests.Instruments.Branches.TC_Bne_un.s_double_2();
            Tests.Instruments.Branches.TC_Bne_un.uint_1();
            Tests.Instruments.Branches.TC_Bne_un.uint_2();
            Tests.Instruments.Branches.TC_Bne_un.ulong_1();
            Tests.Instruments.Branches.TC_Bne_un.ulong_2();
            Tests.Instruments.Branches.TC_Bne_un.object_1();
            Tests.Instruments.Branches.TC_Bne_un.object_2();
            Tests.Instruments.Branches.TC_Bne_un.nint_1();
            Tests.Instruments.Branches.TC_Bne_un.nint_2();
            Tests.Instruments.Branches.TC_Bne_un.float_1();
            Tests.Instruments.Branches.TC_Bne_un.float_2();
            Tests.Instruments.Branches.TC_Bne_un.double_1();
            Tests.Instruments.Branches.TC_Bne_un.double_2();

            var test = new Tests.Instruments.Branches.TC_Bne_un();
            test.CmpInfinite_float();
            test.CmpInfinite_double();
        }
    }
}
