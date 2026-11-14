"""Train a separate fresh/rotten image classifier.

Expected layout: Unified_Dataset/<produce>/{fresh,rotten}/*.{jpg,jpeg,png}.
This artifact is not used by the sensor-only FastAPI endpoint.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}
IMAGE_SIZE = (128, 128)
SEED = 42


def collect_images(data_dir: Path):
    samples = []
    produce_names = set()
    for produce_dir in sorted(path for path in data_dir.iterdir() if path.is_dir()):
        produce_samples = []
        for label, class_name in enumerate(("fresh", "rotten")):
            class_dir = produce_dir / class_name
            if not class_dir.is_dir():
                continue
            produce_samples.extend(
                (path, label, produce_dir.name)
                for path in class_dir.rglob("*")
                if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
            )
        present = {label for _, label, _ in produce_samples}
        if present == {0, 1}:
            samples.extend(produce_samples)
            produce_names.add(produce_dir.name)
    if len(produce_names) < 3:
        raise SystemExit(
            "Need at least 3 produce folders with both fresh/ and rotten/ images "
            "to hold out unseen produce types for validation and testing."
        )
    if not samples:
        raise SystemExit(f"No images found in expected layout under {data_dir}")
    return samples, sorted(produce_names)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_dir", type=Path, help="Path to Unified_Dataset")
    parser.add_argument("--epochs", type=int, default=12)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--weights", choices=("imagenet", "none"), default="imagenet",
                        help="ImageNet initialization may need a one-time network download.")
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parent / "models" / "produce_image_quality.keras")
    args = parser.parse_args()
    data_dir = args.data_dir.resolve()
    if not data_dir.is_dir():
        raise SystemExit(f"Image dataset directory not found: {data_dir}")

    samples, produce_names = collect_images(data_dir)
    randomizer = random.Random(SEED)
    randomizer.shuffle(produce_names)
    test_count = max(1, round(len(produce_names) * 0.2))
    validation_count = max(1, round(len(produce_names) * 0.2))
    if test_count + validation_count >= len(produce_names):
        raise SystemExit("Need more produce folders to make train, validation, and test splits.")
    test_names = set(produce_names[:test_count])
    validation_names = set(produce_names[test_count:test_count + validation_count])
    train_names = set(produce_names) - test_names - validation_names

    import tensorflow as tf
    from tensorflow.keras import layers
    from tensorflow.keras.applications import MobileNetV2
    from tensorflow.keras.applications.mobilenet_v2 import preprocess_input

    def make_dataset(allowed_names, shuffle):
        selected = [(str(p), label) for p, label, produce in samples if produce in allowed_names]
        if not selected or {label for _, label in selected} != {0, 1}:
            raise SystemExit("Each data split must include both fresh and rotten images.")
        paths, labels = zip(*selected)
        ds = tf.data.Dataset.from_tensor_slices((list(paths), list(labels)))
        if shuffle:
            ds = ds.shuffle(len(paths), seed=SEED, reshuffle_each_iteration=True)

        def load_image(path, label):
            image = tf.io.read_file(path)
            image = tf.io.decode_image(image, channels=3, expand_animations=False)
            image.set_shape([None, None, 3])
            image = tf.image.resize(image, IMAGE_SIZE)
            return preprocess_input(image), tf.cast(label, tf.float32)

        return ds.map(load_image, num_parallel_calls=tf.data.AUTOTUNE).batch(
            args.batch_size).prefetch(tf.data.AUTOTUNE), len(paths)

    train_ds, train_count = make_dataset(train_names, True)
    validation_ds, _ = make_dataset(validation_names, False)
    test_ds, test_count = make_dataset(test_names, False)

    base = MobileNetV2(input_shape=(*IMAGE_SIZE, 3), include_top=False,
                       weights=None if args.weights == "none" else "imagenet")
    base.trainable = False
    inputs = tf.keras.Input(shape=(*IMAGE_SIZE, 3))
    features = base(inputs, training=False)
    features = layers.GlobalAveragePooling2D()(features)
    features = layers.Dropout(0.3)(features)
    outputs = layers.Dense(1, activation="sigmoid")(features)
    model = tf.keras.Model(inputs, outputs)
    model.compile(optimizer="adam", loss="binary_crossentropy", metrics=["accuracy"])
    model.fit(train_ds, validation_data=validation_ds, epochs=args.epochs,
              callbacks=[tf.keras.callbacks.EarlyStopping(
                  patience=3, restore_best_weights=True, monitor="val_loss")])
    test_loss, test_accuracy = model.evaluate(test_ds, verbose=0)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    model.save(args.output)
    metadata = {
        "modelType": "image_fresh_rotten_prototype",
        "labels": ["fresh", "rotten"],
        "inputSize": list(IMAGE_SIZE),
        "splitUnit": "produce_folder",
        "trainImages": train_count,
        "testImages": test_count,
        "testLoss": float(test_loss),
        "testAccuracy": float(test_accuracy),
        "testProduceTypes": sorted(test_names),
    }
    args.output.with_suffix(".metadata.json").write_text(
        json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps(metadata, indent=2))
    print(f"Saved separate image model: {args.output}")


if __name__ == "__main__":
    main()
