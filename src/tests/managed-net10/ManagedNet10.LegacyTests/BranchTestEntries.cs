namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunBranchInstructions()
        {
            RunBranchBr();
            RunBranchBrtrue();
            RunBranchBrfalse();
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
    }
}
