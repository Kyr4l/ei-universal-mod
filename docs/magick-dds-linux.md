Use `nvddsinfo` to see DDS files details.
`nvddsinfo filename.dds`

Flipping DDS files with `magick`:
`magick file-in.dds -flip -define dds:mipmaps=0 -define dds:compression=none file-out.dds`

Use `gnu_parallel` for batch processing
`parallel --bar magick {} -flip -define dds:mipmaps=0 -define dds:compression=none {} ::: *.dds`
