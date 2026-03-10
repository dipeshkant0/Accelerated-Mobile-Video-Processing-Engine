package com.example.videoprocessingengine

import android.content.Context
import android.util.Log

object LutParser {

    data class LutData(val data: FloatArray, val size: Int)

    fun parseCubeFile(context: Context, filename: String): LutData? {
        try {
            val inputStream = context.assets.open(filename)
            val reader = inputStream.bufferedReader()

            var lutSize = 0
            val floatList = mutableListOf<Float>()

            reader.forEachLine { line ->
                val trimmed = line.trim()

                // Skip empty lines or comments
                if (trimmed.isEmpty() || trimmed.startsWith("#")) {
                    return@forEachLine
                }

                // 1. Extract the Grid Size
                if (trimmed.startsWith("LUT_3D_SIZE")) {
                    val parts = trimmed.split(Regex("\\s+"))
                    if (parts.size >= 2) {
                        lutSize = parts[1].toInt()
                    }
                }
                // 2. Ignore min/max domain declarations for standard LUTs
                else if (trimmed.startsWith("TITLE") || trimmed.startsWith("DOMAIN_")) {
                    return@forEachLine
                }
                // 3. Extract the actual RGB Float values
                else {
                    val parts = trimmed.split(Regex("\\s+"))
                    if (parts.size >= 3) {
                        try {
                            floatList.add(parts[0].toFloat()) // Red
                            floatList.add(parts[1].toFloat()) // Green
                            floatList.add(parts[2].toFloat()) // Blue
                        } catch (e: NumberFormatException) {
                            Log.w("LutParser", "Skipping unparseable line: $trimmed")
                        }
                    }
                }
            }
            reader.close()

            // Validate that we got exactly the right amount of data expected:
            // Total colors = Size * Size * Size.
            // Total floats = Total colors * 3 (because each color has R, G, B)
            val expectedFloats = (lutSize * lutSize * lutSize) * 3

            if (lutSize == 0 || floatList.size != expectedFloats) {
                Log.e("LutParser", "Invalid LUT format or corrupted data. Expected $expectedFloats floats but got ${floatList.size}")
                return null
            }

            Log.d("LutParser", "Successfully parsed LUT size $lutSize with ${floatList.size} floats.")
            return LutData(floatList.toFloatArray(), lutSize)

        } catch (e: Exception) {
            Log.e("LutParser", "Error reading $filename", e)
            return null
        }
    }
}
