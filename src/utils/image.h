// Copyright (c) 2014, Alexey Ivanov

#pragma once

#include <FreeImagePlus.h>
#include <vector>

namespace ImageUtils
{

enum IMAGEFORMAT { PNG_IMAGE = 0, JPEG_IMAGE, BMP_IMAGE, IMAGE_FORMATS_COUNT };

/*!
   \brief Image class that loaded from HBITMAP and can be saved to std::vector<BYTE> of to file.
   Delayed DLL load technique is used.
*/
class AIMPCoverImage : public fipWinImage
{
public:
    
    //! Creates image from HBITMAP. Bitmap handle lifetime is controlled by release_bitmap argument.
    explicit AIMPCoverImage(HBITMAP cover_bitmap_handle, bool release_bitmap = false, unsigned width = 0, unsigned height = 0); // throws std::runtime_error

    /*!
        \brief Saves image to std::vector<BYTE> container.
        \param image_format ID of required image format. See IMAGEFORMAT for supported formats.
        \param image_data reference to vector for saving.
        \throw std::runtime_error if saving fails.
    */
    void saveToVector(IMAGEFORMAT image_format, std::vector<BYTE>& image_data) const; // throws std::runtime_error

    /*!
        \brief Saves image to file. File format is determined by file name extention.
        \param file_name file name.
        \throw std::runtime_error if saving fails.
    */
    void saveToFile(const std::wstring& file_name) const; // throws std::runtime_error
};

/*!
    \return size of bitmap by handle.
    \throw std::runtime_error if error occured while determine size.
*/
SIZE getBitmapSize(HBITMAP bitmap_handle); // throw std::runtime_error

} // namespace ImageUtils
