import './style.css'

import { decode, toRGBA8 } from 'upng-js'
import imagequant_init, { quantize } from './lib/quantize'
import lz4_init from './lib/lz4compress'

await imagequant_init()
const { lz4compress } = await lz4_init()

const config_folder_input = document.getElementById('config-folder')
const clock_id_input = document.getElementById('clock-id')
const clock_size_input = document.getElementById('clock-size')
const compress_check = document.getElementById('compress')
const idle_check = document.getElementById('idle')
const internal_check = document.getElementById('internal')
const process_btn = document.getElementById('process')
const messages_div = document.getElementById('messages')

// --------------------------------
// Enable dropdowns
for (const dd of document.getElementsByClassName('dd')) {
  dd.firstElementChild.onclick = () => {
    const backdrop = document.createElement('DIV')
    backdrop.style.position = 'fixed'
    backdrop.style.inset = 0
    backdrop.style.zIndex = 100000
    backdrop.onclick = () => {
      dd.classList.remove('opened')
      backdrop.remove()
    }
    dd.appendChild(backdrop)
    dd.classList.add('opened')
  }
}

// --------------------------------
// Show a message at the bottom for a short time
const show_message = (msg, is_error=false) => {
  const msg_div = document.createElement('DIV')
  msg_div.innerText = msg
  msg_div.className = 'msg-' + (is_error ? 'error' : 'info')
  messages_div.appendChild(msg_div)
  setTimeout(() => {
    msg_div.remove()
  }, 5000)
}

// --------------------------------
// Read a JSON file and return it as an Object
const read_json = file => new Promise((resolve, reject) => {
  const reader = new FileReader()
  reader.onload = () => resolve(JSON.parse(reader.result))
  reader.onerror = () => reject('Could not read file: ' + file.name)
  reader.readAsText(file)
})

// --------------------------------
// Join two or more Uint8Arrays
const u8a_join = (...arrs) => {
  let len = 0
  for (const bar of arrs) len += bar.length
  const result = new Uint8Array(len)
  len = 0
  for (const bar of arrs) {
    result.set(bar, len)
    len += bar.length
  }
  return result
}

// --------------------------------
// Take a jpg or gif file and return width, height and bytes payload
const image_data = (img_file, read=true) => new Promise((resolve, reject) => {
  const obj_url = URL.createObjectURL(img_file)
  const img = new Image()

  img.onload = () => {
    // First get image width and height
    const width = img.naturalWidth
    const height = img.naturalHeight
    // Little cleanup
    URL.revokeObjectURL(obj_url)
    if (read) {
      // Now get the file bytes
      const reader = new FileReader()
      reader.onload = () => resolve({
        width,
        height,
        bytes: new Uint8Array(reader.result)
      })
      reader.onerror = () => reject('Could not read image: ' + img_file.name)
      reader.readAsArrayBuffer(img_file)
    }
    else resolve({ width, height, bytes: null })
  }

  img.onerror = () => reject('Could not load image: ' + img_file.name)

  img.src = obj_url
})

// --------------------------------
// Take a png file, quantize it and return width, height and bytes payload in BGRA index8 format
const png2index8 = png_file => new Promise((resolve, reject) => {
  const reader = new FileReader()

  reader.onload = () => {
    const png = decode(reader.result)

    const { indices, palette } = quantize(
      new Uint8Array(toRGBA8(png)[0]), // rgba
      png.width,
      png.height,
      256, // max_colors
      1, // speed: 1..10 (lower = better quality, slower)
      0, // min_quality
      100, // target_quality
      1 // dithering
    )

    resolve({
      width: png.width,
      height: png.height,
      bytes: u8a_join(palette, indices) // Join resulting Uint8Arrays
    })
  }

  reader.onerror = () => reject('Could not read image: ' + png_file.name)

  reader.readAsArrayBuffer(png_file)
})

// --------------------------------
// Process a single file
const file2resdata = async (file, compress=true) => {
  let data, img_type

  // Check file extension
  if (/\.jp(e?)g$/i.test(file.name)) {
    data = await image_data(file)
    img_type = 9 // Raw JPEG payload
  }
  else if (/\.gif$/i.test(file.name)) {
    data = await image_data(file)
    img_type = 3 // Raw GIF payload
  }
  else if (/\.png$/i.test(file.name)) {
    data = await png2index8(file)
    img_type = 75 // index8-like payload (palette + pixels)
  }
  else throw 'Unsupported file type: ' + file.name

  const header = new Uint8Array([
    img_type, (img_type === 75 ? +compress : 0),
    data.bytes.length & 0xFF, (data.bytes.length >> 8) & 0xFF, (data.bytes.length >> 16) & 0xFF,
    data.height & 0xFF,
    ((data.height >> 8) & 0x0F) | ((data.width & 0x0F) << 4),
    (data.width >> 4) & 0xFF,
    0, 0, 0, 0, 0, 0, 0, 0
  ])

  // Check again if file is png and compression is requested
  return u8a_join(header, (img_type === 75 && compress) ? lz4compress(data.bytes) : data.bytes)
}

// --------------------------------
// Unsigned integer to 4-byte big endian Uint8Array
const uint2bigendian = uint => {
  // Check if the input is a valid unsigned 32-bit integer
  if (uint < 0 || uint > 0xFFFFFFFF) {
    throw 'Input must be a valid unsigned 32-bit integer (0 to 4294967295)'
  }
  // Create an ArrayBuffer with a size of 4 bytes
  const buffer = new ArrayBuffer(4)
  // Create a DataView to manipulate the buffer
  const view = new DataView(buffer)
  // Write the unsigned 32-bit integer to the buffer at offset 0, using Big-Endian format
  view.setUint32(0, uint, false) // 'false' specifies Big-Endian order
  // Return the buffer as a Uint8Array
  return new Uint8Array(buffer)
}

// --------------------------------
// Get watch face ID from selected folder
config_folder_input.onchange = ({ target: { files } }) => {
  if (files.length) {
    const folder_match = files[0].webkitRelativePath.split('/')[0].match(/\d+/)
    if (folder_match) clock_id_input.value = folder_match[0]
  }
}

// --------------------------------
// Start processing files in folder (do the magic!)
document.getElementById('upload-form').onsubmit = async event => {
  event.preventDefault()
  process_btn.disabled = true
  try {
    if (!config_folder_input.files.length) throw 'No folder selected or empty folder'

    // Clock ID
    let clock_id = +clock_id_input.value
    if (clock_id < 50000 || clock_id > 65535) throw 'Watch face ID must be in [50000..65535]'

    // Build output file name according to clock id
    const out_file_name = `Clock${clock_id}_res`

    // Make files in list accessible by key
    const files = {}
    for (const file of config_folder_input.files) files[file.name] = file

    // Load config JSON
    const config = await read_json(files['config.json'])

    // Clock size
    let clock_size = +clock_size_input.value
    // Check if auto-detect was selected
    if (!clock_size) {
      // Build sizes object
      const sizes = {}
      for (const opt of clock_size_input.options) sizes[opt.label] = +opt.value
      // Find width & height of the first image in first layer
      // TODO: Allow this for other data types
      const { width, height } = await image_data(files[config[0].imgArr[0]], false)
      const res = `${width}×${height}`
      clock_size = sizes[res]
      if (!clock_size) throw 'Unsupported watchface resolution: ' + res
    }

    // This is the clock id that will be in the generated file
    clock_id |= clock_size
    if (internal_check.checked) clock_id |= 0x80000000

    // Try to find thumbnail
    let thumbnail_filename = ''
    for (const item in files) if (/thumbnail/i.test(item)) {
      thumbnail_filename = item
      break
    }
    if (!thumbnail_filename) throw 'No images named like *thumbnail* in source dir'

    // Process it
    const thumbnail_resdata = await file2resdata(
      files[thumbnail_filename],
      compress_check.checked
    )

    // Now process everything else
    let img_resdata = new Uint8Array()
    let z_img_resdata = new Uint8Array()
    // Here we'll store: 'img_name': [ start_pos, img_size ]
    const img_offsets = {}

    // Add _res data to respective vars
    const add_resdata = async item => {
      if (Array.isArray(item)) {
        await add_resdata(item[item.length - 1]) // Last item should be the file name
      }
      else if (!files[item]) throw `File not in folder - ${item}`
      else if (!img_offsets[item]) { // Check if it was not processed before
        const bytes = await file2resdata(files[item], compress_check.checked)
        // Check if the file should be in the z-image section
        if (/^z_/i.test(item)) {
          img_offsets[item] = [ z_img_resdata.length, bytes.length ]
          z_img_resdata = u8a_join(z_img_resdata, bytes)
        }
        else {
          img_offsets[item] = [ img_resdata.length, bytes.length ]
          img_resdata = u8a_join(img_resdata, bytes)
        }
      }
    }

    let idx = 1 // Current layer counter
    for (const { num, imgArr, drawType } of config) {
      if (num != imgArr.length) throw `Error in layer ${idx}: Wrong number of items in imgArr`
      // Check items in imgArr
      try {
        // drawType 55 uses imgArr: [width, height, "ScreenName", "icon.png"]
        if (drawType === 55) await add_resdata(imgArr[imgArr.length - 1])
        else for (const item of imgArr) await add_resdata(item)
      } catch (er) {
        throw `Error in layer ${idx}: ${er}`
      }
      idx++
    }

    // 32 is the clock thumbnail start address
    const start_offset = 32 + thumbnail_resdata.length
    const z_img_offset = start_offset + img_resdata.length
    let layer_resdata = new Uint8Array()

    // Functions for creating _res data for imgArr items
    const layer_resdata_offsets = filename => {
      layer_resdata = u8a_join(layer_resdata, u8a_join(
        uint2bigendian(img_offsets[filename][0]),
        uint2bigendian(img_offsets[filename][1])
      ))
    }
    const tenc = new TextEncoder()
    const img_arr_item_resdata = item => {
      if (Array.isArray(item)) for (const subitem of item) {
        if (Number.isInteger(subitem)) layer_resdata = u8a_join(
          layer_resdata,
          uint2bigendian(subitem)
        )
        // Check if item is a file name
        else if (/\.(png|jp(e?)g|gif)$/i.test(subitem)) layer_resdata_offsets(subitem)
        // Not a file name, add it as a 30-byte string
        else {
          const arr = new Uint8Array(30)
          arr.set(tenc.encode(subitem.substring(0, 29)))
          layer_resdata = u8a_join(layer_resdata, arr)
        }
      }
      else layer_resdata_offsets(item)
    }

    // Now we build the layer _res data, we need to iterate in config again
    for (const layer of config) {
      layer_resdata = u8a_join(
        layer_resdata,
        uint2bigendian(layer.drawType),
        uint2bigendian(layer.dataType)
      )

      if ([ 130, 59, 52 ].includes(layer.dataType))
        layer_resdata = u8a_join(layer_resdata, uint2bigendian(layer.interval))
      else if (layer.dataType === 112) for (const num of layer.area_num)
        layer_resdata = u8a_join(layer_resdata, uint2bigendian(num))

      layer_resdata = u8a_join(
        layer_resdata,
        uint2bigendian(layer.alignType),
        uint2bigendian(layer.x),
        uint2bigendian(layer.y),
        uint2bigendian(layer.num)
      )

      if ([ 32, 40, 55 ].includes(layer.drawType)) img_arr_item_resdata(layer.imgArr)
      else for (const item of layer.imgArr) img_arr_item_resdata(item)
    }

    // Let's build the full _res data now!
    const resdata = u8a_join(
      // Start _res data with magic string
      tenc.encode(idle_check.checked ? 'II@*24dG' : 'Sb@*O2GG'),
      uint2bigendian(clock_id),
      uint2bigendian(32),
      uint2bigendian(thumbnail_resdata.length),
      uint2bigendian(start_offset),
      uint2bigendian(img_resdata.length),
      uint2bigendian(z_img_offset + z_img_resdata.length),
      thumbnail_resdata,
      img_resdata,
      z_img_resdata,
      layer_resdata
    )
    // Download it
    const url = URL.createObjectURL(
      new Blob([ resdata ], { type: 'application/octet-stream' })
    )
    const anchor = document.createElement('A')
    anchor.style.display = 'none'
    anchor.href = url
    anchor.download = out_file_name
    document.body.appendChild(anchor)
    anchor.click()
    anchor.remove()
    setTimeout(() => URL.revokeObjectURL(url), 1000)
    show_message(`Done! File ${out_file_name} download requested`)
  } catch (err) {
    show_message(err, true)
  }
  process_btn.disabled = false
}
