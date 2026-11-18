// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
package io.github.tmcleroy.satrecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

// The new version's APK, for Android's installer: the launcher downloads it into the app's cache, in update/
// (recomp-common/launcher/src/update.cpp), and LauncherActivity.installPackage() gives the installer this provider's
// address for it, readable by the installer alone (the provider isn't exported; the intent grants it). Only the
// files right in that folder can be read, nothing else of the app's.
public final class UpdateProvider extends ContentProvider {
    static final String MIME = "application/vnd.android.package-archive";

    static String authority(Context context) {
        return context.getPackageName() + ".update";
    }

    static File folder(Context context) {
        return new File(context.getCacheDir(), "update");
    }

    // The address of a file in the folder, or null for one elsewhere
    static Uri uri(Context context, File file) {
        try {
            if (!file.getCanonicalFile().getParentFile().equals(folder(context).getCanonicalFile())) {
                return null;
            }
        } catch (Exception e) {
            return null;
        }
        return new Uri.Builder().scheme("content").authority(authority(context)).appendPath(file.getName()).build();
    }

    private File file(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || uri.getPathSegments().size() != 1 || name.contains("/") || name.startsWith(".")) {
            throw new FileNotFoundException(String.valueOf(uri));
        }
        File file = new File(folder(getContext()), name);
        if (!file.isFile()) {
            throw new FileNotFoundException(String.valueOf(uri));
        }
        return file;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new FileNotFoundException("read only: " + uri);
        }
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return MIME;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File file;
        try {
            file = file(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] columns = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            row[i] = OpenableColumns.DISPLAY_NAME.equals(columns[i]) ? file.getName()
                     : OpenableColumns.SIZE.equals(columns[i])       ? (Object) file.length()
                                                                       : null;
        }
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
