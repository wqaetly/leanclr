using System.Runtime.InteropServices;
using GcTests.Fixtures;

namespace ManagedNet10.LegacyTests
{
    internal static partial class Program
    {
        public static void RunGcRootsAndHandles()
        {
            LegacyTestRunner.RunType(typeof(GcNet10RootHandleTests));
        }

        public static void RunGcRoots()
        {
            RunGcRootsAndHandles();
        }

        public static void RunGcHandles()
        {
            RunGcRootsAndHandles();
        }
    }

    internal sealed class GcNet10RootHandleTests : GcTestCaseBase
    {
        [UnitTest]
        [GcUnitTest]
        public void Static_object_field_remains_reachable_after_collect()
        {
            GcStaticRoots.root = new object();
            try
            {
                FullCollect();
                Assert.NotNull(GcStaticRoots.root);
            }
            finally
            {
                GcStaticRoots.root = null;
            }
        }

        [UnitTest]
        [GcUnitTest]
        public void Static_nested_value_type_field_keeps_child_reachable()
        {
            object child = new object();
            GcStaticRoots.staticNested.leaf.payload = child;
            GCHandle weak = TrackWeak(child);
            try
            {
                child = null;
                FullCollect();
                AssertSurvives(weak);
            }
            finally
            {
                GcStaticRoots.staticNested = default(NestedRefs);
                FreeHandle(ref weak);
            }
        }

        [UnitTest]
        [GcUnitTest]
        public void Strong_handle_keeps_target_reachable()
        {
            object target = new object();
            GCHandle handle = GCHandle.Alloc(target);
            try
            {
                target = null;
                FullCollect();
                Assert.NotNull(handle.Target);
            }
            finally
            {
                FreeHandle(ref handle);
            }
        }

        [UnitTest]
        [GcUnitTest]
        public void Weak_handle_survives_while_strong_handle_exists()
        {
            object target = new object();
            GCHandle strong = GCHandle.Alloc(target);
            GCHandle weak = TrackWeak(target);
            try
            {
                target = null;
                FullCollect();
                Assert.NotNull(strong.Target);
                AssertSurvives(weak);
            }
            finally
            {
                FreeHandle(ref weak);
                FreeHandle(ref strong);
            }
        }

        [UnitTest]
        [GcUnitTest]
        public void Pinned_handle_keeps_byte_array_reachable()
        {
            byte[] data = new byte[] { 1, 2, 3, 4, 5 };
            GCHandle pinned = GCHandle.Alloc(data, GCHandleType.Pinned);
            GCHandle weak = TrackWeak(data);
            try
            {
                data = null;
                FullCollect();
                AssertSurvives(weak);
                Assert.NotNull(pinned.Target);
            }
            finally
            {
                FreeHandle(ref weak);
                FreeHandle(ref pinned);
            }
        }

        [UnitTest]
        [GcUnitTest]
        public void Strong_handle_root_marks_transitive_children()
        {
            object leaf = new object();
            GcNode root = BuildChain(3, leaf);
            GCHandle strong = GCHandle.Alloc(root);
            GCHandle weak = TrackWeak(leaf);
            try
            {
                root = null;
                leaf = null;
                FullCollect();
                AssertSurvives(weak);
            }
            finally
            {
                FreeHandle(ref weak);
                FreeHandle(ref strong);
            }
        }
    }
}
