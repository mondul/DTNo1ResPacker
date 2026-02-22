use wasm_bindgen::prelude::*;
use js_sys::{Object, Reflect, Uint8Array};

fn err_to_js<E: core::fmt::Debug>(e: E) -> JsValue {
    JsValue::from_str(&format!("{e:?}"))
}

#[wasm_bindgen]
pub fn quantize(
    rgba: &[u8],
    width: usize,
    height: usize,
    max_colors: u32,
    speed: i32,
    min_quality: u8,
    target_quality: u8,
    dithering: f32,
) -> Result<JsValue, JsValue> {
    console_error_panic_hook::set_once();

    let expected = width
        .checked_mul(height)
        .and_then(|px| px.checked_mul(4))
        .ok_or_else(|| JsValue::from_str("width*height*4 overflow"))?;

    if rgba.len() != expected {
        return Err(JsValue::from_str(&format!(
            "rgba length mismatch: got {}, expected {} (width*height*4)",
            rgba.len(),
            expected
        )));
    }

    // SAFETY: imagequant::RGBA is #[repr(C)] with 4x u8 fields, so layout matches packed RGBA bytes.
    let rgba_px: &[imagequant::RGBA] = unsafe {
        core::slice::from_raw_parts(rgba.as_ptr() as *const imagequant::RGBA, rgba.len() / 4)
    };

    let mut attr = imagequant::new();
    attr.set_max_colors(max_colors).map_err(err_to_js)?;
    attr.set_speed(speed).map_err(err_to_js)?;
    attr.set_quality(min_quality, target_quality).map_err(err_to_js)?;

    // gamma = 0.0 means input is sRGB (the common case).
    let mut img = attr
        .new_image_borrowed(rgba_px, width, height, 0.0)
        .map_err(err_to_js)?;

    let mut result = attr.quantize(&mut img).map_err(err_to_js)?;
    result
        .set_dithering_level(dithering)
        .map_err(err_to_js)?;

    // This returns exactly what you want: (palette, indices)
    let (palette_rgba, indices) = result.remapped(&mut img).map_err(err_to_js)?;

    // Flatten palette into [r,g,b,a, r,g,b,a, ...]
    let mut palette = Vec::with_capacity(palette_rgba.len() * 4);
    for c in palette_rgba.iter() {
        palette.push(c.r);
        palette.push(c.g);
        palette.push(c.b);
        palette.push(c.a);
    }

    let n_colors = palette.len() / 4;
    let colors_to_use = n_colors.min(256);

    // Always exactly 256 colors * 4 bytes; initialized to 0 => transparent black padding.
    let mut bgra = vec![0u8; 256 * 4];

    for i in 0..colors_to_use {
        let base_in = i * 4;
        let r = palette[base_in + 0];
        let g = palette[base_in + 1];
        let b = palette[base_in + 2];
        let a = palette[base_in + 3];

        let base_out = i * 4;
        bgra[base_out + 0] = b;
        bgra[base_out + 1] = g;
        bgra[base_out + 2] = r;
        bgra[base_out + 3] = a;
    }

    // Return a JS object { indices: Uint8Array, palette: Uint8Array, paletteLength: number }
    let out = Object::new();
    Reflect::set(
        &out,
        &JsValue::from_str("indices"),
        &Uint8Array::from(indices.as_slice()),
    )?;
    Reflect::set(
        &out,
        &JsValue::from_str("palette"),
        &Uint8Array::from(bgra.as_slice()),
    )?;
    Reflect::set(
        &out,
        &JsValue::from_str("paletteLength"),
        &JsValue::from_f64(palette_rgba.len() as f64),
    )?;

    Ok(out.into())
}
