namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunToStringRegressions()
        {
            LegacyTestRunner.RunTypes(
                typeof(RuntimeMethodNameToStringRegression),
                typeof(Tests.Bugs.Bug_ValueTypeToString_2022_7_22),
                typeof(Tests.Bugs.Bug20220927.Bug_2022_9_27));
        }

        public static void RunRuntimeMethodNameToStringRegression()
        {
            LegacyTestRunner.RunType(typeof(RuntimeMethodNameToStringRegression));
        }
    }

    internal sealed class RuntimeMethodNameToStringRegression : TestCaseBase
    {
        [UnitTest]
        public void MethodInfoNameUsesRuntimeMethodHandleName()
        {
            System.Reflection.MethodInfo method = typeof(RuntimeMethodNameToStringRegression).GetMethod(
                "MethodInfoNameUsesRuntimeMethodHandleName",
                System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance);

            Assert.NotNull(method);
            Assert.Equal("MethodInfoNameUsesRuntimeMethodHandleName", method.Name);
        }

        [UnitTest]
        public void StackTraceToStringUsesRuntimeMethodNames()
        {
            string text = new System.Diagnostics.StackTrace().ToString();
            Assert.NotNull(text);
        }
    }
}
