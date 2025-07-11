// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace System.Runtime.InteropServices
{
    /// <summary>
    /// An unsafe class that provides a set of methods to access the underlying data representations of collections.
    /// </summary>
    public static class CollectionsMarshal
    {
        /// <summary>
        /// Get a <see cref="Span{T}"/> view over a <see cref="List{T}"/>'s data.
        /// Items should not be added or removed from the <see cref="List{T}"/> while the <see cref="Span{T}"/> is in use.
        /// </summary>
        /// <param name="list">The list to get the data view over.</param>
        /// <typeparam name="T">The type of the elements in the list.</typeparam>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static Span<T> AsSpan<T>(List<T>? list)
        {
            Span<T> span = default;
            if (list is not null)
            {
                int size = list._size;
                T[] items = list._items;
                Debug.Assert(items is not null, "Implementation depends on List<T> always having an array.");

                if ((uint)size > (uint)items.Length)
                {
                    // List<T> was erroneously mutated concurrently with this call, leading to a count larger than its array.
                    ThrowHelper.ThrowInvalidOperationException_ConcurrentOperationsNotSupported();
                }

                Debug.Assert(typeof(T[]) == list._items.GetType(), "Implementation depends on List<T> always using a T[] and not U[] where U : T.");
                span = new Span<T>(ref MemoryMarshal.GetArrayDataReference(items), size);
            }

            return span;
        }

        /// <summary>
        /// Get a <see cref="Span{Byte}"/> view over a <see cref="BitArray"/>'s data.
        /// </summary>
        /// <param name="array">The <see cref="BitArray"/> whose backing storage should be viewed.</param>
        /// <remarks>
        /// <para>
        /// The <see cref="BitArray"/> may have more capacity than is required to store the number of bits represented by
        /// <see cref="BitArray.Length"/>. The returned span's <see cref="Span{Byte}.Length"/> will be the smallest number
        /// of bytes capable of representing that length. If the <see cref="BitArray"/>'s length is not evenly divisible by
        /// 8, the last byte of the span may contain extraneous bits that do not represent elements in the <see cref="BitArray"/>.
        /// These may be ignored.
        /// </para>
        /// <para>
        /// The length of the <see cref="BitArray"/> should not be changed while the resulting <see cref="Span{Byte}"/>
        /// is in use. After such a change, the span may no longer refer to the <see cref="BitArray"/>'s backing storage.
        /// </para>
        /// </remarks>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static Span<byte> AsBytes(BitArray? array) =>
            array is null ? default :
            array._array.AsSpan(0, BitArray.GetByteArrayLengthFromBitLength(array.Length));

        /// <summary>
        /// Gets either a ref to a <typeparamref name="TValue"/> in the <see cref="Dictionary{TKey, TValue}"/> or a ref null if it does not exist in the <paramref name="dictionary"/>.
        /// </summary>
        /// <param name="dictionary">The dictionary to get the ref to <typeparamref name="TValue"/> from.</param>
        /// <param name="key">The key used for lookup.</param>
        /// <typeparam name="TKey">The type of the keys in the dictionary.</typeparam>
        /// <typeparam name="TValue">The type of the values in the dictionary.</typeparam>
        /// <remarks>
        /// Items should not be added or removed from the <see cref="Dictionary{TKey, TValue}"/> while the ref <typeparamref name="TValue"/> is in use.
        /// The ref null can be detected using System.Runtime.CompilerServices.Unsafe.IsNullRef
        /// </remarks>
        public static ref TValue GetValueRefOrNullRef<TKey, TValue>(Dictionary<TKey, TValue> dictionary, TKey key) where TKey : notnull
            => ref dictionary.FindValue(key);

        /// <summary>
        /// Gets either a ref to a <typeparamref name="TValue"/> in the <see cref="Dictionary{TKey, TValue}"/> or a ref null if it does not exist in the <paramref name="dictionary"/>.
        /// </summary>
        /// <param name="dictionary">The dictionary to get the ref to <typeparamref name="TValue"/> from.</param>
        /// <param name="key">The key used for lookup.</param>
        /// <typeparam name="TKey">The type of the keys in the dictionary.</typeparam>
        /// <typeparam name="TValue">The type of the values in the dictionary.</typeparam>
        /// <typeparam name="TAlternateKey">The type of an alternate key for lookups in the dictionary.</typeparam>
        /// <remarks>
        /// Items should not be added or removed from the <see cref="Dictionary{TKey, TValue}"/> while the ref <typeparamref name="TValue"/> is in use.
        /// The ref null can be detected using System.Runtime.CompilerServices.Unsafe.IsNullRef
        /// </remarks>
        public static ref TValue GetValueRefOrNullRef<TKey, TValue, TAlternateKey>(Dictionary<TKey, TValue>.AlternateLookup<TAlternateKey> dictionary, TAlternateKey key)
            where TKey : notnull
            where TAlternateKey : notnull, allows ref struct
            => ref dictionary.FindValue(key, out _);

        /// <summary>
        /// Gets a ref to a <typeparamref name="TValue"/> in the <see cref="Dictionary{TKey, TValue}"/>, adding a new entry with a default value if it does not exist in the <paramref name="dictionary"/>.
        /// </summary>
        /// <param name="dictionary">The dictionary to get the ref to <typeparamref name="TValue"/> from.</param>
        /// <param name="key">The key used for lookup.</param>
        /// <param name="exists">Whether or not a new entry for the given key was added to the dictionary.</param>
        /// <typeparam name="TKey">The type of the keys in the dictionary.</typeparam>
        /// <typeparam name="TValue">The type of the values in the dictionary.</typeparam>
        /// <remarks>Items should not be added to or removed from the <see cref="Dictionary{TKey, TValue}"/> while the ref <typeparamref name="TValue"/> is in use.</remarks>
        public static ref TValue? GetValueRefOrAddDefault<TKey, TValue>(Dictionary<TKey, TValue> dictionary, TKey key, out bool exists) where TKey : notnull
            => ref Dictionary<TKey, TValue>.CollectionsMarshalHelper.GetValueRefOrAddDefault(dictionary, key, out exists);

        /// <summary>
        /// Gets a ref to a <typeparamref name="TValue"/> in the <see cref="Dictionary{TKey, TValue}.AlternateLookup{TAlternateKey}"/>, adding a new entry with a default value if it does not exist in the <paramref name="dictionary"/>.
        /// </summary>
        /// <param name="dictionary">The dictionary to get the ref to <typeparamref name="TValue"/> from.</param>
        /// <param name="key">The key used for lookup.</param>
        /// <param name="exists">Whether or not a new entry for the given key was added to the dictionary.</param>
        /// <typeparam name="TKey">The type of the keys in the dictionary.</typeparam>
        /// <typeparam name="TValue">The type of the values in the dictionary.</typeparam>
        /// <typeparam name="TAlternateKey">The type of the alternate key in the dictionary lookup.</typeparam>
        /// <remarks>Items should not be added to or removed from the <see cref="Dictionary{TKey, TValue}.AlternateLookup{TAlternateKey}"/> while the ref <typeparamref name="TValue"/> is in use.</remarks>
        public static ref TValue? GetValueRefOrAddDefault<TKey, TValue, TAlternateKey>(Dictionary<TKey, TValue>.AlternateLookup<TAlternateKey> dictionary, TAlternateKey key, out bool exists)
            where TKey : notnull
            where TAlternateKey : notnull, allows ref struct
            => ref dictionary.GetValueRefOrAddDefault(key, out exists);

        /// <summary>
        /// Sets the count of the <see cref="List{T}"/> to the specified value.
        /// </summary>
        /// <param name="list">The list to set the count of.</param>
        /// <param name="count">The value to set the list's count to.</param>
        /// <typeparam name="T">The type of the elements in the list.</typeparam>
        /// <exception cref="NullReferenceException">
        /// <paramref name="list"/> is <see langword="null"/>.
        /// </exception>
        /// <exception cref="ArgumentOutOfRangeException">
        /// <paramref name="count"/> is negative.
        /// </exception>
        /// <remarks>
        /// When increasing the count, uninitialized data is being exposed.
        /// </remarks>
        public static void SetCount<T>(List<T> list, int count)
        {
            if (count < 0)
            {
                ThrowHelper.ThrowArgumentOutOfRangeException_NeedNonNegNum(nameof(count));
            }

            list._version++;

            if (count > list.Capacity)
            {
                list.Grow(count);
            }
            else if (count < list._size && RuntimeHelpers.IsReferenceOrContainsReferences<T>())
            {
                Array.Clear(list._items, count, list._size - count);
            }

            list._size = count;
        }
    }
}
