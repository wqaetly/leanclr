using GcTests.Collection;
using GcTests.Handles;
using GcTests.Roots;
using GcTests.Scan;

namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunGcRootsAndHandles()
        {
            RunGcRoots();
            RunGcHandles();
        }

        public static void RunGcCollection()
        {
            LegacyTestRunner.RunType(typeof(TC_GC_Collection));
        }

        public static void RunGcRoots()
        {
            LegacyTestRunner.RunType(typeof(TC_GC_Roots));
            RunGcStaticBitmapBitZero();
        }

        public static void RunGcStaticBitmapBitZero()
        {
            LegacyTestRunner.RunType(typeof(TC_GC_StaticBitmapBitZero));
        }

        public static void RunGcHandles()
        {
            LegacyTestRunner.RunType(typeof(TC_GC_Handles));
        }

        public static void RunGcScanGraph()
        {
            LegacyTestRunner.RunType(typeof(TC_GC_Graph));
        }
    }
}
