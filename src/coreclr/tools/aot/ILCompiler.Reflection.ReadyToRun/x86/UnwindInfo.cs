// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Text;

namespace ILCompiler.Reflection.ReadyToRun.x86
{
    /// <summary>
    /// based on <a href="https://github.com/dotnet/runtime/blob/main/src/coreclr/inc/win64unwind.h">src\inc\win64unwind.h</a> _UNWIND_INFO
    /// </summary>
    public class UnwindInfo : BaseUnwindInfo
    {
        public uint FunctionLength { get; set; }

        public UnwindInfo() { }

        public UnwindInfo(NativeReader imageReader, int offset)
        {
            int startOffset = offset;
            FunctionLength = imageReader.DecodeUnsignedGc(ref offset);
            Size = offset - startOffset;
        }

        public override string ToString()
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine($"    FunctionLength: {FunctionLength}");
            return sb.ToString();
        }
    }
}
