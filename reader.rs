use flate2::bufread::MultiGzDecoder;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr, CString};
use std::fs::File;
use std::io::{self, BufRead, BufReader, Read};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

const INPUT_BUFFER: usize = 256 * 1024;
const OUTPUT_BUFFER: usize = 1024 * 1024;

pub struct Reader {
    stream: BufReader<Box<dyn Read>>,
    error: CString,
    code: c_int,
}

impl Reader {
    fn fail(&mut self, error: io::Error) {
        self.error = CString::new(error.to_string().replace(char::from(0), "?")).unwrap();
        self.code = -3;
    }
}

// The C caller owns each opaque pointer and passes valid buffers. No reader is shared.
#[no_mangle]
pub unsafe extern "C" fn fast_gzopen(path: *const c_char, _mode: *const c_char) -> *mut Reader {
    let path = Path::new(std::ffi::OsStr::from_bytes(CStr::from_ptr(path).to_bytes()));
    let file = match File::open(path) {
        Ok(file) => file,
        Err(_) => return std::ptr::null_mut(),
    };
    let mut input = BufReader::with_capacity(INPUT_BUFFER, file);
    let gzip = match input.fill_buf() {
        Ok(data) => data.starts_with(&[0x1f, 0x8b]),
        Err(_) => return std::ptr::null_mut(),
    };
    let decoded: Box<dyn Read> = if gzip {
        Box::new(MultiGzDecoder::new(input))
    } else {
        Box::new(input)
    };
    Box::into_raw(Box::new(Reader {
        stream: BufReader::with_capacity(OUTPUT_BUFFER, decoded),
        error: CString::default(),
        code: 0,
    }))
}

#[no_mangle]
pub unsafe extern "C" fn fast_gzread(
    reader: *mut Reader,
    data: *mut c_void,
    size: c_uint,
) -> c_int {
    let reader = &mut *reader;
    if reader.code != 0 {
        return -1;
    }
    let out = std::slice::from_raw_parts_mut(data.cast::<u8>(), size as usize);
    loop {
        match reader.stream.read(out) {
            Ok(size) => return size as c_int,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => {
                reader.fail(error);
                return -1;
            }
        }
    }
}

// Consume unselected payloads directly from the decoder buffer, without copying them.
#[no_mangle]
pub unsafe extern "C" fn fast_gzskip(reader: *mut Reader, mut size: u64) -> c_int {
    let reader = &mut *reader;
    if reader.code != 0 {
        return -1;
    }
    while size != 0 {
        let count = match reader.stream.fill_buf() {
            Ok([]) => return -1,
            Ok(data) => (data.len() as u64).min(size) as usize,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => {
                reader.fail(error);
                return -1;
            }
        };
        reader.stream.consume(count);
        size -= count as u64;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn fast_gzerror(reader: *mut Reader, code: *mut c_int) -> *const c_char {
    let reader = &*reader;
    if !code.is_null() {
        *code = reader.code;
    }
    reader.error.as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn fast_gzclose(reader: *mut Reader) -> c_int {
    drop(Box::from_raw(reader));
    0
}
#[no_mangle]
pub unsafe extern "C" fn fast_gzfinish(reader: *mut Reader) -> c_int {
    let reader = &mut *reader;
    if reader.code != 0 {
        return -1;
    }
    match io::copy(&mut reader.stream, &mut io::sink()) {
        Ok(_) => 0,
        Err(error) => {
            reader.fail(error);
            -1
        }
    }
}
