using Mono.Linker.Tests.Cases.Expectations.Assertions;
using Mono.Linker.Tests.Cases.Expectations.Metadata;
using Mono.Linker.Tests.Cases.References.Dependencies;

namespace Mono.Linker.Tests.Cases.References
{
    /// <summary>
    /// We can't detect the using usage in the assembly.  As a result, nothing in `library` is going to be marked and that assembly will be deleted.
    /// With the assembly action of `save`, we remove unused assembly references from the assembly and rewrite it.
    /// When cecil writes the assembly, the unused typeref is not written out.
    /// </summary>

    // Add a custom step which sets the assembly action of the test to "save"    
    [SetupCompileBefore("SetSaveAction.dll", new[] { "Dependencies/CustomMarkHandlerSaveAssembly.cs" },
        new[] { "illink.dll", "Mono.Cecil.dll", "netstandard.dll" })]
    [SetupLinkerArgument("--custom-step", "CustomMarkHandlerSaveAssembly,SetSaveAction.dll")]

    // When csc is used, `saved.dll` will have a reference to `library.dll`
    [SetupCompileBefore("library.dll", new[] { "Dependencies/AssemblyOnlyUsedByUsing_Lib.cs" })]
    [SetupCompileBefore("saved.dll", new[] { "Dependencies/AssemblyOnlyUsedByUsing_UnusedUsing.cs" },
        // Even though this testcase doesn't link symbols, tell the compiler to produce symbols to confirm that the behavior
        // isn't affected by the presence of symbols when not passing '-b'.
        new[] { "library.dll" }, additionalArguments: new string[] { "/debug:portable" }, compilerToUse: "csc")]

    // Here to assert that the test is setup correctly to preserve unused code in the saved assembly.  This is an important aspect of the bug
    [KeptMemberInAssembly("saved.dll", typeof(AssemblyOnlyUsedByUsing_UnusedUsing), "Unused()")]

    // The library should be gone. The `using` statement leaves no traces in the IL so nothing in `library` will be marked
    [RemovedAssembly("library.dll")]
    // The `save` action results in the reference to System.Runtime being resolved into a reference directly to System.Private.CoreLib.
    // The reference to `library` is removed.
    [KeptReferencesInAssembly("saved.dll", new[] { "System.Private.CoreLib" })]
    public class AssemblyOnlyUsedByUsingSaveAction
    {
        public static void Main()
        {
            // Use something to keep the reference at compile time
            AssemblyOnlyUsedByUsing_UnusedUsing.UsedToKeepReference();
        }
    }
}
